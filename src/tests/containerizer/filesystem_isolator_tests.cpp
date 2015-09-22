/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>

#include <mesos/mesos.hpp>

#include <process/owned.hpp>
#include <process/gtest.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "slave/paths.hpp"

#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/isolators/filesystem/linux.hpp"
#endif

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/provisioner/paths.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/provisioner.hpp"
#include "tests/containerizer/rootfs.hpp"

using namespace process;

using std::string;
using std::vector;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Launcher;
#ifdef __linux__
using mesos::internal::slave::LinuxFilesystemIsolatorProcess;
using mesos::internal::slave::LinuxLauncher;
#endif
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::Slave;

using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class LinuxFilesystemIsolatorTest: public MesosTest
{
public:
  // This helper creates a MesosContainerizer instance that uses the
  // LinuxFilesystemIsolator. The filesystem isolator takes a
  // TestAppcProvisioner which provisions APPC images by copying files
  // from the host filesystem.
  // 'images' is a map of imageName -> rootfsPath.
  // TODO(xujyan): The current assumption of one rootfs per image name
  // is inconsistent with the real provisioner and we should fix it.
  Try<Owned<MesosContainerizer>> createContainerizer(
      const slave::Flags& flags,
      const hashmap<string, string>& images)
  {
    // Create the root filesystems.
    hashmap<string, Shared<Rootfs>> rootfses;
    foreachpair (const string& imageName, const string& rootfsPath, images) {
      Try<Owned<Rootfs>> rootfs = LinuxRootfs::create(rootfsPath);

      if (rootfs.isError()) {
        return Error("Failed to create LinuxRootfs: " + rootfs.error());
      }

      rootfses.put(imageName, rootfs.get().share());
    }

    Owned<Provisioner> provisioner(new TestProvisioner(rootfses));

    Try<Isolator*> _isolator =
      LinuxFilesystemIsolatorProcess::create(flags, provisioner);

    if (_isolator.isError()) {
      return Error(
          "Failed to create LinuxFilesystemIsolatorProcess: " +
          _isolator.error());
    }

    Owned<Isolator> isolator(_isolator.get());

    Try<Launcher*> _launcher = LinuxLauncher::create(flags);

    if (_launcher.isError()) {
      return Error("Failed to create LinuxLauncher: " + _launcher.error());
    }

    Owned<Launcher> launcher(_launcher.get());

    return Owned<MesosContainerizer>(
        new MesosContainerizer(
            flags,
            true,
            &fetcher,
            launcher,
            {isolator}));
  }

  ContainerInfo createContainerInfo(
      const Option<string>& imageName = None(),
      const vector<Volume>& volumes = vector<Volume>())
  {
    ContainerInfo info;
    info.set_type(ContainerInfo::MESOS);

    if (imageName.isSome()) {
      Image* image = info.mutable_mesos()->mutable_image();
      image->set_type(Image::APPC);

      Image::Appc* appc = image->mutable_appc();
      appc->set_name(imageName.get());
    }

    foreach (const Volume& volume, volumes) {
      info.add_volumes()->CopyFrom(volume);
    }

    return info;
  }

  Volume createVolumeFromHostPath(
      const string& containerPath,
      const string& hostPath,
      const Volume::Mode& mode)
  {
    return CREATE_VOLUME(containerPath, hostPath, mode);
  }

  Volume createVolumeFromImage(
      const string& containerPath,
      const string& imageName,
      const Volume::Mode& mode)
  {
    Volume volume;
    volume.set_container_path(containerPath);
    volume.set_mode(mode);

    Image* image = volume.mutable_image();
    image->set_type(Image::APPC);

    Image::Appc* appc = image->mutable_appc();
    appc->set_name(imageName);

    return volume;
  }

private:
  Fetcher fetcher;
};


// This test verifies that the root filesystem of the container is
// properly changed to the one that's provisioned by the provisioner.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ChangeRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "[ ! -d '" + os::getcwd() + "' ]");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


TEST_F(LinuxFilesystemIsolatorTest, ROOT_Metrics)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  // Use a long running task so we can reliably capture the moment it's alive.
  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "sleep 1000");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count(
      "containerizer/mesos/filesystem/containers_new_rootfs"));
  EXPECT_EQ(
      1, stats.values["containerizer/mesos/filesystem/containers_new_rootfs"]);

  containerizer.get()->destroy(containerId);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Executor was killed.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(9, wait.get().status());
}


// This test verifies that a volume with a relative host path is
// properly created in the container's sandbox and is properly mounted
// in the container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromSandbox)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", "tmp", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(directory, "tmp", "file")));
}


// This test verifies that a volume with an absolute host path as
// well as an absolute container path is properly mounted in the
// container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHost)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d /tmp/sandbox");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", os::getcwd(), Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that a volume with an absolute host path and a
// relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHostSandboxMountPoint)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d mountpoint/sandbox");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("mountpoint", os::getcwd(), Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that persistent volumes are properly mounted in
// the container's root filesystem.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > volume/file");

  executor.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  // Create a persistent volume.
  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithoutRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > volume/file");

  executor.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  executor.mutable_container()->CopyFrom(createContainerInfo());

  // Create a persistent volume.
  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  // To make sure the sandbox directory has the container ID in its
  // path so it doesn't get unmounted by the launcher.
  string directory = slave::paths::createExecutorDirectory(
      flags.work_dir,
      SlaveID(),
      FrameworkID(),
      executor.executor_id(),
      containerId);

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is not specified.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ImageInVolumeWithoutRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(rootfsesDir, "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d rootfs/bin");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      None(),
      {createVolumeFromImage("rootfs", "test_image", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is specified.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ImageInVolumeWithRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  string rootfsesDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(
        flags,
        {{"test_image_rootfs", path::join(rootfsesDir, "test_image_rootfs")},
         {"test_image_volume", path::join(rootfsesDir, "test_image_volume")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "[ ! -d '" + os::getcwd() + "' ] && [ -d rootfs/bin ]");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image_rootfs",
      {createVolumeFromImage("rootfs", "test_image_volume", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that multiple containers with images can be
// launched simultaneously with no interference.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_MultipleContainers)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId1;
  containerId1.set_value(UUID::random().toString());

  ContainerID containerId2;
  containerId2.set_value(UUID::random().toString());

  string rootfsesDir1 = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId1);

  string rootfsesDir2 = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId2);

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(
        flags,
        {{"test_image1", path::join(rootfsesDir1, "test_image1")},
         {"test_image2", path::join(rootfsesDir2, "test_image2")}});

  ASSERT_SOME(containerizer);

  SlaveID slaveId;
  slaveId.set_value("test_slave");

  // First launch container 1 which has a long running task which
  // guarantees that its work directory mount is in the host mount
  // table when container 2 is launched.
  ExecutorInfo executor1 = CREATE_EXECUTOR_INFO(
      "test_executor1",
      "sleep 1000"); // Long running task.

  executor1.mutable_container()->CopyFrom(createContainerInfo("test_image1"));

  // Create a persistent volume for container 1. We do this because
  // we want to test container 2 cleaning up multiple mounts.
  executor1.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory1 = slave::paths::createExecutorDirectory(
      flags.work_dir,
      slaveId,
      DEFAULT_FRAMEWORK_INFO.id(),
      executor1.executor_id(),
      containerId1);

  Future<bool> launch1 = containerizer.get()->launch(
      containerId1,
      executor1,
      directory1,
      None(),
      slaveId,
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  AWAIT_READY(launch1);

  // Now launch container 2 which will copy the host mount table with
  // container 1's work directory mount in it.
  ExecutorInfo executor2 = CREATE_EXECUTOR_INFO(
      "test_executor2",
      "[ ! -d '" + os::getcwd() + "' ]");

  executor2.mutable_container()->CopyFrom(createContainerInfo("test_image2"));

  string directory2 = slave::paths::createExecutorDirectory(
      flags.work_dir,
      slaveId,
      DEFAULT_FRAMEWORK_INFO.id(),
      executor2.executor_id(),
      containerId2);

  Future<bool> launch2 = containerizer.get()->launch(
      containerId2,
      executor2,
      directory2,
      None(),
      slaveId,
      PID<Slave>(),
      false);

  AWAIT_READY(launch2);

  containerizer.get()->destroy(containerId1);

  // Wait on the containers.
  Future<containerizer::Termination> wait1 =
    containerizer.get()->wait(containerId1);
  Future<containerizer::Termination> wait2 =
    containerizer.get()->wait(containerId2);

  AWAIT_READY(wait1);
  AWAIT_READY(wait2);

  // Executor 1 was forcefully killed.
  EXPECT_TRUE(wait1.get().has_status());
  EXPECT_EQ(9, wait1.get().status());

  // Executor 2 exited normally.
  EXPECT_TRUE(wait2.get().has_status());
  EXPECT_EQ(0, wait2.get().status());
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
