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

#include <stout/format.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/volume/utils.hpp"

#include "tests/cluster.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;

using std::map;
using std::string;
using std::vector;

using testing::TestParamInfo;
using testing::Values;
using testing::WithParamInterface;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::volume::HOST_PATH_WHITELIST_DELIM;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

class VolumeHostPathIsolatorTest : public MesosTest {};


// This test verifies that a volume with an absolute host path as
// well as an absolute container path is properly mounted in the
// container's mount namespace.
TEST_F(VolumeHostPathIsolatorTest, ROOT_VolumeFromHost)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -d /tmp/dir");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("/tmp", sandbox.get(), Volume::RW)}));

  string dir = path::join(sandbox.get(), "dir");
  ASSERT_SOME(os::mkdir(dir));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a container launched with a
// rootfs cannot write to a read-only HOST_PATH volume.
TEST_F(VolumeHostPathIsolatorTest, ROOT_ReadOnlyVolumeFromHost)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > /tmp/dir/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("/tmp", sandbox.get(), Volume::RO)}));

  string dir = path::join(sandbox.get(), "dir");
  ASSERT_SOME(os::mkdir(dir));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_NE(0, wait->get().status());
}


// This test verifies that a file volume with an absolute host
// path as well as an absolute container path is properly mounted
// in the container's mount namespace.
TEST_F(VolumeHostPathIsolatorTest, ROOT_FileVolumeFromHost)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -f /tmp/test/file.txt");

  string file = path::join(sandbox.get(), "file");
  ASSERT_SOME(os::touch(file));

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("/tmp/test/file.txt", file, Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ_FOR(
      Containerizer::LaunchResult::SUCCESS, launch, Seconds(60));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a volume with an absolute host path and a
// relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(VolumeHostPathIsolatorTest, ROOT_VolumeFromHostSandboxMountPoint)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -d mountpoint/dir");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("mountpoint", sandbox.get(), Volume::RW)}));

  string dir = path::join(sandbox.get(), "dir");
  ASSERT_SOME(os::mkdir(dir));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that a file volume with an absolute host path
// and a relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(VolumeHostPathIsolatorTest, ROOT_FileVolumeFromHostSandboxMountPoint)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "test -f mountpoint/file.txt");

  string file = path::join(sandbox.get(), "file");
  ASSERT_SOME(os::touch(file));

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("mountpoint/file.txt", file, Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ_FOR(
      Containerizer::LaunchResult::SUCCESS, launch, Seconds(60));

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


// This test verifies that non-existing host paths under whitelist paths
// specified by the `host_path_volume_force_creation` agent flag can be
// properly created and mounted into container's mount namespace.
TEST_F(VolumeHostPathIsolatorTest, ROOT_VolumeFromHostForceCreation)
{
  const string imageName = "test-image";
  const string registryPath = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registryPath, imageName));

  const string storePath = path::join(sandbox.get(), "store");
  const string mntPath = path::join(sandbox.get(), "mnt");
  const vector<string> whitelistedPaths = {
    path::join(mntPath, "whitelist-01"),
    path::join(mntPath, "whitelist-02")
  };

  slave::Flags flags = CreateSlaveFlags();
  flags.docker_registry = registryPath;
  flags.docker_store_dir = storePath;
  flags.image_providers = "docker";
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.host_path_volume_force_creation = strings::join(
      HOST_PATH_WHITELIST_DELIM, whitelistedPaths);

  Fetcher fetcher(flags);

  const Try<MesosContainerizer*> create =
      MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  const string sandboxPath = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(sandboxPath));

  // Specify non-existing paths.
  const vector<string> hostPaths = {
    path::join(mntPath, "whitelist-01", "a", "b", "c"),
    path::join(mntPath, "whitelist-02", "d", "e", "f")
  };

  // Ensure none of `hostPaths` exists.
  foreach (const string& hostPath, hostPaths) {
    ASSERT_FALSE(os::exists(hostPath));
  }

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo = createContainerInfo(imageName, {
      createVolumeHostPath("/mnt/foo", hostPaths.front(), Volume::RW),
      createVolumeHostPath("/mnt/bar", hostPaths.back(), Volume::RW)});

  ExecutorInfo executor = createExecutorInfo(
      "test-executor",
      "test -d /mnt/foo -a -d /mnt/bar");
  executor.mutable_container()->CopyFrom(containerInfo);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, sandboxPath),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
      containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  // Expect `hostPath` was created and is a directory.
  foreach (const string& hostPath, hostPaths) {
    EXPECT_TRUE(os::stat::isdir(hostPath));
  }
}


TEST_F(VolumeHostPathIsolatorTest, ROOT_MountPropagation)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  string mountDirectory = path::join(flags.work_dir, "mount_directory");
  string mountPoint = path::join(mountDirectory, "mount_point");
  string filePath = path::join(mountPoint, "foo");

  ASSERT_SOME(os::mkdir(mountPoint));

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      strings::format(
          "mount -t tmpfs tmpfs %s; touch %s",
          mountPoint,
          filePath).get());

  executor.mutable_container()->CopyFrom(createContainerInfo(
      None(),
      {createVolumeHostPath(
          mountDirectory,
          mountDirectory,
          Volume::RW,
          MountPropagation::BIDIRECTIONAL)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_READY(launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  // If the mount propagation has been setup properly, we should see
  // the file we touch'ed in 'mountPoint'.
  EXPECT_TRUE(os::exists(filePath));
}


class VolumeHostPathIsolatorMesosTest
  : public MesosTest,
    public WithParamInterface<ParamExecutorType> {};


INSTANTIATE_TEST_CASE_P(
    ExecutorType,
    VolumeHostPathIsolatorMesosTest,
    Values(
        ParamExecutorType::commandExecutor(),
        ParamExecutorType::defaultExecutor()),
    ParamExecutorType::Printer());


// This test verifies that the framework can launch a command task
// that specifies both container image and host volumes.
TEST_P(VolumeHostPathIsolatorMesosTest, ROOT_ChangeRootFilesystem)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

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
      Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "test -f /tmp/testfile && test -d " +
      path::join(flags.sandbox_directory, "relative_dir"));

  task.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeHostPath("/tmp", dir1, Volume::RW),
       createVolumeHostPath("relative_dir", dir2, Volume::RW)}));

  if (GetParam().isCommandExecutor()) {
    driver.acceptOffers(
        {offer.id()},
        {LAUNCH({task})});
  } else if (GetParam().isDefaultExecutor()) {
    ExecutorInfo executor;
    executor.mutable_executor_id()->set_value("default");
    executor.set_type(ExecutorInfo::DEFAULT);
    executor.mutable_framework_id()->CopyFrom(frameworkId.get());
    executor.mutable_resources()->CopyFrom(
        Resources::parse("cpus:0.1;mem:32;disk:32").get());

    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task);

    driver.acceptOffers(
        {offer.id()},
        {LAUNCH_GROUP(executor, taskGroup)});
  } else {
    FAIL() << "Unexpected executor type";
  }

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
