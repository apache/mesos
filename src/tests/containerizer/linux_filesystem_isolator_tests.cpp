// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <vector>

#include <mesos/mesos.hpp>

#include <process/owned.hpp>
#include <process/gtest.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "linux/fs.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"

#include "tests/cluster.hpp"
#include "tests/environment.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;
using process::PID;

using std::map;
using std::string;
using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::LinuxFilesystemIsolatorProcess;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace tests {

class LinuxFilesystemIsolatorTest : public MesosTest
{
protected:
  Fetcher fetcher;
};


// This test verifies that the root filesystem of the container is
// properly changed to the one that's provisioned by the provisioner.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ChangeRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "[ ! -d '" + sandbox.get() + "' ]");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that the metrics about the number of executors
// that have root filesystem specified is correctly reported.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_Metrics)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Use a long running task so we can reliably capture the moment it's alive.
  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "sleep 1000");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count(
      "containerizer/mesos/filesystem/containers_new_rootfs"));
  EXPECT_EQ(
      1, stats.values["containerizer/mesos/filesystem/containers_new_rootfs"]);

  containerizer->destroy(containerId);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait->get().status());
}


// This test verifies that a volume with a relative host path is
// properly created in the container's sandbox and is properly mounted
// in the container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromSandbox)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", "tmp", Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(directory, "tmp", "file")));
}


// This test verifies that a volume with an absolute host path as
// well as an absolute container path is properly mounted in the
// container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHost)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -d /tmp/dir");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", sandbox.get(), Volume::RW)}));

  string dir = path::join(sandbox.get(), "dir");
  ASSERT_SOME(os::mkdir(dir));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a file volume with an absolute host
// path as well as an absolute container path is properly mounted
// in the container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_FileVolumeFromHost)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -f /tmp/test/file.txt");

  string file = path::join(sandbox.get(), "file");
  ASSERT_SOME(os::touch(file));

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp/test/file.txt", file, Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY_FOR(launch, Seconds(60));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a volume with an absolute host path and a
// relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHostSandboxMountPoint)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -d mountpoint/dir");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("mountpoint", sandbox.get(), Volume::RW)}));

  string dir = path::join(sandbox.get(), "dir");
  ASSERT_SOME(os::mkdir(dir));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a file volume with an absolute host path
// and a relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_FileVolumeFromHostSandboxMountPoint)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -f mountpoint/file.txt");

  string file = path::join(sandbox.get(), "file");
  ASSERT_SOME(os::touch(file));

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("mountpoint/file.txt", file, Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY_FOR(launch, Seconds(60));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that persistent volumes are properly mounted in
// the container's root filesystem.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
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
      flags.work_dir,
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


// This test verifies that persistent volumes are properly mounted if
// the container does not specify a root filesystem.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithoutRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > volume/file");

  executor.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  // Create a persistent volume.
  string volume = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


// This test verifies that multiple containers with images can be
// launched simultaneously with no interference.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_MultipleContainers)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image1"));
  AWAIT_READY(DockerArchive::create(registry, "test_image2"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId1;
  containerId1.set_value(UUID::random().toString());

  ContainerID containerId2;
  containerId2.set_value(UUID::random().toString());

  ExecutorInfo executor1 = createExecutorInfo(
      "test_executor1",
      "sleep 1000");

  executor1.mutable_container()->CopyFrom(createContainerInfo("test_image1"));

  // Create a persistent volume for container 1. We do this because
  // we want to test container 2 cleaning up multiple mounts.
  executor1.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  string volume = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory1 = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory1));

  Future<bool> launch1 = containerizer->launch(
      containerId1,
      None(),
      executor1,
      directory1,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch1);

  ExecutorInfo executor2 = createExecutorInfo(
      "test_executor2",
      "[ ! -d '" + sandbox.get() + "' ]");

  executor2.mutable_container()->CopyFrom(createContainerInfo("test_image2"));

  string directory2 = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory2));

  Future<bool> launch2 = containerizer->launch(
      containerId2,
      None(),
      executor2,
      directory2,
      None(),
      SlaveID(),
      map<string, string>(),
      false);

  AWAIT_READY(launch1);

  // Wait on the containers.
  Future<Option<ContainerTermination>> wait1 =
    containerizer->wait(containerId1);

  Future<Option<ContainerTermination>> wait2 =
    containerizer->wait(containerId2);

  containerizer->destroy(containerId1);

  AWAIT_READY(wait1);
  ASSERT_SOME(wait1.get());
  ASSERT_TRUE(wait1->get().has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait1->get().status());

  AWAIT_READY(wait2);
  ASSERT_SOME(wait2.get());
  ASSERT_TRUE(wait2->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait2->get().status());
}


// This test verifies the case where we don't need a bind mount for
// slave's working directory because the mount containing it is
// already a shared mount in its own peer group.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_WorkDirMountNotNeeded)
{
  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Make 'sandbox' a shared mount in its own peer group.
  ASSERT_SOME(os::shell(
      "mount --bind %s %s && "
      "mount --make-private %s &&"
      "mount --make-shared %s",
      directory->c_str(),
      directory->c_str(),
      directory->c_str(),
      directory->c_str()));

  // Slave's working directory is under 'sandbox'.
  slave::Flags flags = CreateSlaveFlags();
  flags.work_dir = path::join(directory.get(), "slave");

  ASSERT_SOME(os::mkdir(flags.work_dir));

  Try<Isolator*> isolator = LinuxFilesystemIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  // Verifies that there's no mount for slave's working directory.
  bool mountFound = false;
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == flags.work_dir) {
      mountFound = true;
    }
  }

  EXPECT_FALSE(mountFound);

  delete isolator.get();
}


// This test verifies the case where we do need a bind mount for
// slave's working directory because the mount containing it is not a
// shared mount in its own peer group.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_WorkDirMountNeeded)
{
  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Make 'sandbox' a private mount.
  ASSERT_SOME(os::shell(
      "mount --bind %s %s && "
      "mount --make-private %s",
      directory->c_str(),
      directory->c_str(),
      directory->c_str()));

  slave::Flags flags = CreateSlaveFlags();
  flags.work_dir = path::join(directory.get(), "slave");

  ASSERT_SOME(os::mkdir(flags.work_dir));

  Try<Isolator*> isolator = LinuxFilesystemIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  bool mountFound = false;
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == flags.work_dir) {
      EXPECT_SOME(entry.shared());
      mountFound = true;
    }
  }

  EXPECT_TRUE(mountFound);

  delete isolator.get();
}


// End to end Mesos integration tests for linux filesystem isolator.
class LinuxFilesystemIsolatorMesosTest : public LinuxFilesystemIsolatorTest {};


// This test verifies that the framework can launch a command task
// that specifies a container image.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_ChangeRootFilesystemCommandExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -d " + flags.sandbox_directory);

  task.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  driver.launchTasks(offer.id(), {task});

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the framework can launch a command task
// that specifies both container image and host volumes.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_ChangeRootFilesystemCommandExecutorWithHostVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  // Preparing two volumes:
  // - host_path: dir1, container_path: /tmp
  // - host_path: dir2, container_path: relative_dir
  string dir1 = path::join(sandbox.get(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  string testFile = path::join(dir1, "testfile");
  ASSERT_SOME(os::touch(testFile));

  string dir2 = path::join(sandbox.get(), "dir2");
  ASSERT_SOME(os::mkdir(dir2));

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -f /tmp/testfile && test -d " +
      path::join(flags.sandbox_directory, "relative_dir"));

  task.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", dir1, Volume::RW),
       createVolumeFromHostPath("relative_dir", dir2, Volume::RW)}));

  driver.launchTasks(offer.id(), {task});

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the framework can launch a command task
// that specifies both container image and persistent volumes.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_ChangeRootFilesystemCommandExecutorPersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem:1024;disk(role1):1024";
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  Offer offer = offers.get()[0];

  string dir1 = path::join(sandbox.get(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      frameworkInfo.principal());

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:512").get() + persistentVolume,
      "echo abc > path1/file");

  task.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", dir1, Volume::RW)}));

  // Create the persistent volumes and launch task via `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume), LAUNCH({task})},
      filters);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // NOTE: The command executor's id is the same as the task id.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  string directory = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      offer.slave_id(),
      frameworkId.get(),
      executorId);

  EXPECT_FALSE(os::exists(path::join(directory, "path1")));

  string volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      "role1",
      "id1");

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volumePath, "file")));

  driver.stop();
  driver.join();
}


// This test verifies that persistent volumes are unmounted properly
// after a checkpointed framework disappears and the slave restarts.
//
// TODO(jieyu): Even though the command task specifies a new
// filesystem root, the executor (command executor) itself does not
// change filesystem root (uses the host filesystem). We need to add a
// test to test the scenario that the executor itself changes rootfs.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_RecoverOrphanedPersistentVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem:1024;disk(role1):1024";
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  string dir1 = path::join(sandbox.get(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      frameworkInfo.principal());

  // Create a task that does nothing for a long time.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:512").get() + persistentVolume,
      "sleep 1000");

  task.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", dir1, Volume::RW)}));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(DoDefault());

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  // Create the persistent volumes and launch task via `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume), LAUNCH({task})});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *containers->begin();

  // Restart the slave.
  slave.get()->terminate();

  // Wipe the slave meta directory so that the slave will treat the
  // above running task as an orphan.
  ASSERT_SOME(os::rmdir(slave::paths::getMetaRootDir(flags.work_dir)));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Recreate the containerizer using the same helper as above.
  containerizer.reset();

  create = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(create);

  containerizer.reset(create.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  AWAIT_READY(_recover);

  // Wait until the orphan containers are cleaned up.
  AWAIT_READY(containerizer->wait(containerId));

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  // All mount targets should be under this directory.
  string directory = slave::paths::getSandboxRootDir(flags.work_dir);

  // Verify that the orphaned container's persistent volume and
  // the rootfs are unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    EXPECT_FALSE(strings::contains(entry.target, directory))
      << "Target was not unmounted: " << entry.target;
  }

  driver.stop();
  driver.join();
}


// This test verifies that the environment variables for sandbox
// (i.e., MESOS_SANDBOX) is set properly.
TEST_F(LinuxFilesystemIsolatorMesosTest, ROOT_SandboxEnvironmentVariable)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      strings::format(
          "if [ \"$MESOS_SANDBOX\" != \"%s\" ]; then exit 1; fi &&"
          "if [ ! -d \"$MESOS_SANDBOX\" ]; then exit 1; fi",
          flags.sandbox_directory).get());

  task.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  driver.launchTasks(offer.id(), {task});

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the volume usage accounting for sandboxes
// with bind-mounted volumes (while linux filesystem isolator is used)
// works correctly by creating a file within the volume the size of
// which exceeds the sandbox quota.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_VolumeUsageExceedsSandboxQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem:128;disk(role1):128";
  flags.isolation = "disk/du,filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  flags.container_disk_watch_interval = Milliseconds(1);
  flags.enforce_container_disk_quota = true;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  // We request a sandbox (1MB) that is smaller than the persistent
  // volume (4MB) and attempt to create a file in that volume that is
  // twice the size of the sanbox (2MB).
  Resources volume = createPersistentVolume(
      Megabytes(4),
      "role1",
      "id1",
      "volume_path",
      None(),
      None(),
      frameworkInfo.principal());

  Resources taskResources =
      Resources::parse("cpus:1;mem:64;disk(role1):1").get() + volume;

  // We sleep to give quota enforcement (du) a chance to kick in.
  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      taskResources,
      "dd if=/dev/zero of=volume_path/file bs=1048576 count=2 && sleep 1");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.acceptOffers(
      {offers.get()[0].id()},
      {CREATE(volume),
      LAUNCH({task})});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// Tests that the task fails when it attempts to write to a persistent volume
// mounted as read-only. Note that although we use a shared persistent volume,
// the behavior is the same for non-shared persistent volumes.
TEST_F(LinuxFilesystemIsolatorMesosTest,
       ROOT_WriteAccessSharedPersistentVolumeReadOnlyMode)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem:128;disk(role1):128";
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  // We create a shared volume which shall be used by the task to
  // write to that volume.
  Resource volume = createPersistentVolume(
      Megabytes(4),
      "role1",
      "id1",
      "volume_path",
      None(),
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  // The task uses the shared volume as read-only.
  Resource roVolume = volume;
  roVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RO);

  Resources taskResources =
    Resources::parse("cpus:1;mem:64;disk(role1):1").get() + roVolume;

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      taskResources,
      "echo hello > volume_path/file");

  // The task fails to write to the volume since the task's resources
  // intends to use the volume as read-only.
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFailed;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.acceptOffers(
      {offers.get()[0].id()},
      {CREATE(volume),
       LAUNCH({task})});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFailed);
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
