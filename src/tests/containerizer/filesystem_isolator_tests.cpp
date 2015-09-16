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

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "slave/paths.hpp"

#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/isolators/filesystem/linux.hpp"
#endif

#include "slave/containerizer/mesos/containerizer.hpp"

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
  Try<Owned<MesosContainerizer>> createContainerizer(
      const slave::Flags& flags,
      const vector<string>& imageNames)
  {
    // Create the root filesystems.
    hashmap<string, Shared<Rootfs>> rootfses;
    foreach (const string& imageName, imageNames) {
      Try<Owned<Rootfs>> rootfs =
        LinuxRootfs::create(path::join(os::getcwd(), "rootfses", imageName));

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

    Try<Launcher*> _launcher =
      LinuxLauncher::create(flags, isolator->namespaces().get().get());

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

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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


// This test verifies that a volume with a relative host path is
// properly created in the container's sandbox and is properly mounted
// in the container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromSandbox)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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
  flags.work_dir = os::getcwd();

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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
  flags.work_dir = os::getcwd();

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is not specified.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ImageInVolumeWithoutRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, {"test_image_rootfs", "test_image_volume"});

  ASSERT_SOME(containerizer);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

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
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
