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

#include <unistd.h>

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include <mesos/mesos.hpp>

#include <mesos/slave/containerizer.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "tests/mesos.hpp"

using std::string;

using process::Future;
using process::Owned;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::containerizer::paths::getSandboxPath;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class NamespacesIsolatorTest : public MesosTest
{
public:
  void SetUp() override
  {
    MesosTest::SetUp();

    directory = os::getcwd(); // We're inside a temporary sandbox.
    containerId.set_value(id::UUID::random().toString());
  }

  Try<Owned<MesosContainerizer>> createContainerizer(
      const string& isolation,
      const Option<bool>& disallowSharingAgentPidNamespace = None(),
      const Option<bool>& disallowSharingAgentIpcNamespace = None(),
      const Option<Bytes>& defaultContainerShmSize = None())
  {
    slave::Flags flags = CreateSlaveFlags();
    flags.image_providers = "docker";
    flags.isolation = isolation + ",docker/runtime";

    if (disallowSharingAgentPidNamespace.isSome()) {
      flags.disallow_sharing_agent_pid_namespace =
        disallowSharingAgentPidNamespace.get();
    }

    if (disallowSharingAgentIpcNamespace.isSome()) {
      flags.disallow_sharing_agent_ipc_namespace =
        disallowSharingAgentIpcNamespace.get();
    }

    flags.default_container_shm_size = defaultContainerShmSize;

    fetcher.reset(new Fetcher(flags));

    Try<MesosContainerizer*> _containerizer =
      MesosContainerizer::create(flags, true, fetcher.get());

    if (_containerizer.isError()) {
      return Error(_containerizer.error());
    }

    return Owned<MesosContainerizer>(_containerizer.get());
  }

  // Read a uint64_t value from the given path.
  Try<uint64_t> readValue(const string& path)
  {
    Try<string> value = os::read(path);

    if (value.isError()) {
      return Error("Failed to read '" + path + "': " + value.error());
    }

    return numify<uint64_t>(strings::trim(value.get()));
  }

  string directory;
  Owned<Fetcher> fetcher;
  ContainerID containerId;
};


TEST_F(NamespacesIsolatorTest, ROOT_PidNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/pid");

  ASSERT_SOME(containerizer);

  // Write the command's pid namespace inode and init name to files.
  const string command =
    "stat -Lc %i /proc/1/ns/pid > ns && (cat /proc/1/cmdline > init)";

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        createContainerConfig(
            None(),
            createExecutorInfo("executor", command),
            directory),
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the container.
  Future<Option<ContainerTermination>> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that the command was run in a different pid namespace.
  Result<ino_t> testPidNamespace = ns::getns(::getpid(), "pid");
  ASSERT_SOME(testPidNamespace);

  Try<string> containerPidNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerPidNamespace);

  EXPECT_NE(stringify(testPidNamespace.get()),
            strings::trim(containerPidNamespace.get()));

  // Check that the word 'mesos' is the part of the name for the
  // container's 'init' process. This verifies that /proc has been
  // correctly mounted for the container.
  Try<string> init = os::read(path::join(directory, "init"));
  ASSERT_SOME(init);

  EXPECT_TRUE(strings::contains(init.get(), "mesos"));
}


// This test verifies a top-level container can share pid namespace
// with the agent when the field `share_pid_namespace` is set as
// true in `ContainerInfo.linux_info`. Please note that the agent flag
// `--disallow_sharing_agent_pid_namespace` is set to
// false by default, that means top-level container is allowed to share
// pid namespace with agent.
TEST_F(NamespacesIsolatorTest, ROOT_SharePidNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/pid");

  ASSERT_SOME(containerizer);

  // Write the command's pid namespace inode to file.
  const string command = "stat -Lc %i /proc/1/ns/pid > ns";

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", command),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_share_pid_namespace(true);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the container.
  Future<Option<ContainerTermination>> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that the command was run in the same pid namespace.
  Result<ino_t> testPidNamespace = ns::getns(::getpid(), "pid");
  ASSERT_SOME(testPidNamespace);

  Try<string> containerPidNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerPidNamespace);

  EXPECT_EQ(stringify(testPidNamespace.get()),
            strings::trim(containerPidNamespace.get()));
}


// This test verifies launching a top-level container to share
// pid namespace with agent will fail when the agent flag
// `--disallow_sharing_agent_pid_namespace` is set to true.
TEST_F(NamespacesIsolatorTest, ROOT_SharePidNamespaceWhenDisallow)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/pid", true);

  ASSERT_SOME(containerizer);

  const string command = "sleep 1000";

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", command),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_share_pid_namespace(true);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_FAILED(launch);
}


// This test verifies that when `namespaces/ipc` isolator is enabled and
// container's IPC mode is not set, for backward compatibility we will
// keep the previous behavior: Top level container will have its own IPC
// namespace and nested container will share the IPC namespace with its
// parent container. If the container does not have its own rootfs, it
// will share agent's /dev/shm, otherwise it will have its own /dev/shm.
TEST_F(NamespacesIsolatorTest, ROOT_IPCNamespaceWithIPCModeUnset)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Value we will set the top-level container's IPC namespace shmmax to.
  uint64_t shmmaxValue = static_cast<uint64_t>(::getpid());

  Try<uint64_t> hostShmmax = readValue("/proc/sys/kernel/shmmax");
  ASSERT_SOME(hostShmmax);

  // Verify that the host namespace shmmax is different.
  ASSERT_NE(hostShmmax.get(), shmmaxValue);

  // Launch a top-level container with IPC mode
  // unset and it does not have its own rootfs.
  string command =
    "stat -Lc %i /proc/self/ns/ipc > ns && "
    "echo " + stringify(shmmaxValue) + " > /proc/sys/kernel/shmmax && "
    "cp /proc/sys/kernel/shmmax /dev/shm/shmmax && "
    "sleep 1000";

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        createContainerConfig(
            None(),
            createExecutorInfo("executor", command),
            directory),
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Since the top-level container does not have its own
  // rootfs, it will share host's /dev/shm, so let's wait
  // until /dev/shm/shmmax is created in the host.
  Duration waited = Duration::zero();

  do {
    if (os::exists("/dev/shm/shmmax")) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch a nested container with IPC mode unset and it has its own rootfs.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  mesos::Image image;
  image.set_type(mesos::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  command =
    "stat -Lc %i /proc/self/ns/ipc > ns && "
    "test `cat /proc/sys/kernel/shmmax` = " + stringify(shmmaxValue) + " && "
    "touch /dev/shm/file";

  launch = containerizer.get()->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo(command), containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the nested container.
  Future<Option<ContainerTermination>> wait =
    containerizer.get()->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that top-level container and the nested container are in the
  // same IPC namespace but not in the same IPC namespace with host.
  Result<ino_t> testIPCNamespace = ns::getns(::getpid(), "ipc");
  ASSERT_SOME(testIPCNamespace);

  Try<string> containerIPCNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerIPCNamespace);

  Try<string> nestedcontainerIPCNamespace =
    os::read(path::join(getSandboxPath(directory, nestedContainerId), "ns"));

  ASSERT_SOME(nestedcontainerIPCNamespace);

  EXPECT_NE(stringify(testIPCNamespace.get()),
            strings::trim(containerIPCNamespace.get()));

  EXPECT_EQ(strings::trim(containerIPCNamespace.get()),
            strings::trim(nestedcontainerIPCNamespace.get()));

  // The nested container will have its own /dev/shm since it has its own
  // rootfs, so the file it created should not exist in the host.
  ASSERT_FALSE(os::exists("/dev/shm/file"));

  // Check that we modified the IPC shmmax of the namespace, not the host.
  Try<uint64_t> containerShmmax = readValue("/dev/shm/shmmax");
  ASSERT_SOME(containerShmmax);

  // Verify that we didn't modify shmmax in the host namespace.
  ASSERT_EQ(hostShmmax.get(), readValue("/proc/sys/kernel/shmmax").get());

  EXPECT_NE(hostShmmax.get(), containerShmmax.get());
  EXPECT_EQ(shmmaxValue, containerShmmax.get());

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  ASSERT_SOME(os::rm("/dev/shm/shmmax"));
}


// This test verifies that when `namespaces/ipc` isolator is not enabled,
// for backward compatibility we will keep the previous behavior: Any
// containers will share IPC namespace with agent, and if the container
// does not have its own rootfs, it will also share agent's /dev/shm,
// otherwise it will have its own /dev/shm.
TEST_F(NamespacesIsolatorTest, ROOT_IPCNamespaceWithIPCIsolatorDisabled)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux");

  ASSERT_SOME(containerizer);

  // Launch a top-level container which does not have its own rootfs.
  string command =
    "stat -Lc %i /proc/self/ns/ipc > ns && "
    "touch /dev/shm/root && "
    "sleep 1000";

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        createContainerConfig(
            None(),
            createExecutorInfo("executor", command),
            directory),
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Since the top-level container does not have its own
  // rootfs, it will share host's /dev/shm, so let's wait
  // until /dev/shm/root is created in the host.
  Duration waited = Duration::zero();

  do {
    if (os::exists("/dev/shm/root")) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch a nested container which has its own rootfs.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  mesos::Image image;
  image.set_type(mesos::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  command =
    "stat -Lc %i /proc/self/ns/ipc > ns && "
    "touch /dev/shm/nested";

  launch = containerizer.get()->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo(command), containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait on the nested container.
  Future<Option<ContainerTermination>> wait =
    containerizer.get()->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that top-level container and the nested container are
  // in the same IPC namespace with host.
  Result<ino_t> testIPCNamespace = ns::getns(::getpid(), "ipc");
  ASSERT_SOME(testIPCNamespace);

  Try<string> containerIPCNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerIPCNamespace);

  Try<string> nestedcontainerIPCNamespace =
    os::read(path::join(getSandboxPath(directory, nestedContainerId), "ns"));

  ASSERT_SOME(nestedcontainerIPCNamespace);

  EXPECT_EQ(stringify(testIPCNamespace.get()),
            strings::trim(containerIPCNamespace.get()));

  EXPECT_EQ(strings::trim(containerIPCNamespace.get()),
            strings::trim(nestedcontainerIPCNamespace.get()));

  // The nested container will have its own /dev/shm since it has its own
  // rootfs, so the file it created should not exist in the host.
  ASSERT_FALSE(os::exists("/dev/shm/nested"));

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  ASSERT_SOME(os::rm("/dev/shm/root"));
}


// This test verifies that a top-level container with private IPC mode will
// have its own IPC namespace and /dev/shm, and it can share IPC namespace
// and /dev/shm with its child containers, grandchild containers and debug
// containers.
TEST_F(NamespacesIsolatorTest, ROOT_ShareIPCNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Launch a top-level container with `PRIVATE` IPC mode and 128MB /dev/shm,
  // check its /dev/shm size is correctly set and its IPC namespace is
  // different than agent's IPC namespace, write its IPC namespace inode to
  // a file under /dev/shm.
  const string command =
    "df -m /dev/shm | grep -w 128 && "
    "test `stat -Lc %i /proc/self/ns/ipc` != `stat -Lc %i /proc/1/ns/ipc` && "
    "stat -Lc %i /proc/self/ns/ipc > /dev/shm/root && "
    "touch marker && "
    "sleep 1000";

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", command),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_ipc_mode(LinuxInfo::PRIVATE);
  container->mutable_linux_info()->set_shm_size(128);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait until the marker file is created.
  Duration waited = Duration::zero();

  do {
    if (os::exists(path::join(directory, "marker"))) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // The file created by the top-level container should only exist in
  // its own /dev/shm rather than in agent's /dev/shm.
  ASSERT_FALSE(os::exists("/dev/shm/root"));

  // Now launch two child containers with `SHARE_PARENT` ipc mode.
  ContainerID childContainerId1, childContainerId2;

  childContainerId1.mutable_parent()->CopyFrom(containerId);
  childContainerId1.set_value(id::UUID::random().toString());

  childContainerId2.mutable_parent()->CopyFrom(containerId);
  childContainerId2.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_linux_info()->set_ipc_mode(LinuxInfo::SHARE_PARENT);

  // Launch the first child container, check its /dev/shm size is 128MB, it
  // can see the file created by its parent container in /dev/shm and it is
  // in the same IPC namespace with its parent container, and then write its
  // IPC namespace inode to a file under /dev/shm.
  launch = containerizer.get()->launch(
      childContainerId1,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/root` && "
              "stat -Lc %i /proc/self/ns/ipc > /dev/shm/child1 && "
              "touch marker && "
              "sleep 1000"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait until the marker file is created.
  waited = Duration::zero();
  const string childSandboxPath1 = getSandboxPath(directory, childContainerId1);

  do {
    if (os::exists(path::join(childSandboxPath1, "marker"))) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch the second child container with its own rootfs, check its /dev/shm
  // size is 128MB, it can see the files created by its parent container and the
  // first child container in /dev/shm and it is in the same IPC namespace with
  // its parent container and the first child container, and then write its IPC
  // namespace inode to a file under /dev/shm.
  mesos::Image image;
  image.set_type(mesos::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  launch = containerizer.get()->launch(
      childContainerId2,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/root` && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/child1` && "
              "stat -Lc %i /proc/self/ns/ipc > /dev/shm/child2 && "
              "touch marker && "
              "sleep 1000"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait until the marker file is created.
  waited = Duration::zero();
  const string childSandboxPath2 = getSandboxPath(directory, childContainerId2);

  do {
    if (os::exists(path::join(childSandboxPath2, "marker"))) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch a grandchild container with `SHARE_PARENT` ipc mode under the first
  // child container, check its /dev/shm size is 128MB, it can see the files
  // created by its parent and grandparent containers and it is in the same IPC
  // namespace with its parent and grandparent containers.
  ContainerID grandchildContainerId;
  grandchildContainerId.mutable_parent()->CopyFrom(childContainerId1);
  grandchildContainerId.set_value(id::UUID::random().toString());

  launch = containerizer.get()->launch(
      grandchildContainerId,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/child1` && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/root`"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer.get()->wait(grandchildContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Launch a debug container under the first child container, check its
  // /dev/shm size is 128MB and it is in the same IPC namespace with its
  // parent container.
  ContainerID debugContainerId1;
  debugContainerId1.mutable_parent()->CopyFrom(childContainerId1);
  debugContainerId1.set_value(id::UUID::random().toString());

  containerInfo.clear_mesos();
  containerInfo.clear_linux_info();

  launch = containerizer.get()->launch(
      debugContainerId1,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/child1`"),
          containerInfo,
          ContainerClass::DEBUG),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  wait = containerizer.get()->wait(debugContainerId1);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Launch another debug container under the second child container, check its
  // /dev/shm size is 128MB and it is in the same IPC namespace with its parent
  // container.
  ContainerID debugContainerId2;
  debugContainerId2.mutable_parent()->CopyFrom(childContainerId2);
  debugContainerId2.set_value(id::UUID::random().toString());

  launch = containerizer.get()->launch(
      debugContainerId2,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 && "
              "test `stat -Lc %i /proc/self/ns/ipc` = `cat /dev/shm/child2`"),
          containerInfo,
          ContainerClass::DEBUG),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  wait = containerizer.get()->wait(debugContainerId2);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that nested container with private IPC mode will
// have its own IPC namespace and /dev/shm.
TEST_F(NamespacesIsolatorTest, ROOT_PrivateIPCNamespace)
{
  // Create containerizer with `--default_container_shm_size=64MB`.
  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      "filesystem/linux,namespaces/ipc",
      None(),
      None(),
      Megabytes(64));

  ASSERT_SOME(containerizer);

  // Launch a top-level container with `PRIVATE` IPC mode, check its /dev/shm
  // size is correctly set to the default value, touch a file in its /dev/shm,
  // and write its IPC namespace inode to a file in its sandbox.
  const string command =
    "df -m /dev/shm | grep -w 64 && "
    "touch /dev/shm/root &&"
    "stat -Lc %i /proc/self/ns/ipc > ns && "
    "sleep 1000";

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", command),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_ipc_mode(LinuxInfo::PRIVATE);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait until the `ns` file is created in the sandbox.
  Duration waited = Duration::zero();

  do {
    if (os::exists(path::join(directory, "ns"))) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch a nested container with `PRIVATE` IPC mode, check its /dev/shm
  // size is correctly set to the default value and the file created by the
  // top-level container does not exist in its /dev/shm, touch a file in its
  // /dev/shm and write its IPC namespace inode to a file in its sandbox.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_linux_info()->set_ipc_mode(LinuxInfo::PRIVATE);

  launch = containerizer.get()->launch(
      nestedContainerId1,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 64 &&"
              "test ! -e /dev/shm/root &&"
              "touch /dev/shm/nested1 &&"
              "stat -Lc %i /proc/self/ns/ipc > ns && "
              "sleep 1000"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait until the `ns` file is created in the sandbox.
  waited = Duration::zero();
  const string nestedSandboxPath1 =
    getSandboxPath(directory, nestedContainerId1);

  do {
    if (os::exists(path::join(nestedSandboxPath1, "ns"))) {
      break;
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  EXPECT_LT(waited, process::TEST_AWAIT_TIMEOUT);

  // Launch another nested container with private IPC mode and 128MB
  // /dev/shm, check its /dev/shm size is correctly set to 128MB and
  // the files created by the top-level container and the first nested
  // container do not exist in its /dev/shm, write its IPC namespace
  // inode to a file in its sandbox.
  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  containerInfo.mutable_linux_info()->set_shm_size(128);

  launch = containerizer.get()->launch(
      nestedContainerId2,
      createContainerConfig(
          createCommandInfo(
              "df -m /dev/shm | grep -w 128 &&"
              "test ! -e /dev/shm/root &&"
              "test ! -e /dev/shm/nested1 &&"
              "stat -Lc %i /proc/self/ns/ipc > ns"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer.get()->wait(
      nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Check top-level container and the two nested containers
  // have their own IPC namespaces.
  Try<uint64_t> rootIpcNamespace = readValue(path::join(directory, "ns"));
  ASSERT_SOME(rootIpcNamespace);

  Try<uint64_t> nestedIpcNamespace1 =
    readValue(path::join(nestedSandboxPath1, "ns"));

  ASSERT_SOME(nestedIpcNamespace1);

  Try<uint64_t> nestedIpcNamespace2 =
    readValue(path::join(getSandboxPath(directory, nestedContainerId2), "ns"));

  ASSERT_SOME(nestedIpcNamespace2);

  EXPECT_NE(rootIpcNamespace.get(), nestedIpcNamespace1.get());
  EXPECT_NE(rootIpcNamespace.get(), nestedIpcNamespace2.get());
  EXPECT_NE(nestedIpcNamespace1.get(), nestedIpcNamespace2.get());

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


// This test verifies that top-level container and nested
// containers can share agent's IPC namespace and /dev/shm.
TEST_F(NamespacesIsolatorTest, ROOT_ShareAgentIPCNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Launch a top-level container with `SHARE_PARENT` IPC mode,
  // write its IPC namespace inode to a file under /dev/shm.
  const string command =
    "stat -Lc %i /proc/self/ns/ipc > /dev/shm/root && "
    "sleep 1000";

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", command),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_ipc_mode(LinuxInfo::SHARE_PARENT);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Launch a nested container with `SHARE_PARENT` IPC mode,
  // write its IPC namespace inode to a file under /dev/shm.
  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_linux_info()->set_ipc_mode(LinuxInfo::SHARE_PARENT);

  launch = containerizer.get()->launch(
      nestedContainerId1,
      createContainerConfig(
          createCommandInfo("stat -Lc %i /proc/self/ns/ipc > /dev/shm/nest1"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer.get()->wait(
      nestedContainerId1);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Launch another nested container with `SHARE_PARENT` IPC mode and its
  // own rootfs, write its IPC namespace inode to a file under /dev/shm.
  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  mesos::Image image;
  image.set_type(mesos::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  launch = containerizer.get()->launch(
      nestedContainerId2,
      createContainerConfig(
          createCommandInfo("stat -Lc %i /proc/self/ns/ipc > /dev/shm/nest2"),
          containerInfo),
      std::map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  wait = containerizer.get()->wait(nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  // Check top-level container and the two nested containers
  // share agent's IPC namespace and /dev/shm.
  Try<uint64_t> rootIpcNamespace = readValue("/dev/shm/root");
  ASSERT_SOME(rootIpcNamespace);

  Try<uint64_t> nestedIpcNamespace1 = readValue("/dev/shm/nest1");
  ASSERT_SOME(nestedIpcNamespace1);

  Try<uint64_t> nestedIpcNamespace2 = readValue("/dev/shm/nest2");
  ASSERT_SOME(nestedIpcNamespace2);

  Result<ino_t> agentIpcNamespace = ns::getns(::getpid(), "ipc");
  ASSERT_SOME(agentIpcNamespace);

  EXPECT_EQ(rootIpcNamespace.get(), agentIpcNamespace.get());
  EXPECT_EQ(nestedIpcNamespace1.get(), agentIpcNamespace.get());
  EXPECT_EQ(nestedIpcNamespace2.get(), agentIpcNamespace.get());

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());

  ASSERT_SOME(os::rm("/dev/shm/root"));
  ASSERT_SOME(os::rm("/dev/shm/nest1"));
  ASSERT_SOME(os::rm("/dev/shm/nest2"));
}


// This test verifies that top-level container with `SHARE_PARENT` IPC mode
// will fail to launch when `--disallow_sharing_agent_ipc_namespace = true`.
TEST_F(NamespacesIsolatorTest, ROOT_DisallowShareAgentIPCNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc", None(), true);

  ASSERT_SOME(containerizer);

  // Launch a top-level container with `SHARE_PARENT` IPC mode.
  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", "sleep 1000"),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_ipc_mode(LinuxInfo::SHARE_PARENT);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_FAILED(launch);
}


// This test verifies that we do not support specifying container's
// /dev/shm size when its IPC mode is not `PRIVATE`.
TEST_F(NamespacesIsolatorTest, ROOT_NonePrivateIPCModeWithShmSize)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Launch a container with a specified /dev/shm size but without
  // specifying IPC mode.
  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  mesos::slave::ContainerConfig containerConfig = createContainerConfig(
      None(),
      createExecutorInfo("executor", "sleep 1000"),
      directory);

  ContainerInfo* container = containerConfig.mutable_container_info();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_linux_info()->set_shm_size(128);

  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId1,
        containerConfig,
        std::map<string, string>(),
        None());

  AWAIT_FAILED(launch);

  // Launch another container with a specified /dev/shm size and `SHARE_PARENT`
  // IPC mode.
  ContainerID containerId2;
  containerId2.set_value(id::UUID::random().toString());

  container->mutable_linux_info()->set_ipc_mode(LinuxInfo::SHARE_PARENT);

  launch = containerizer.get()->launch(
      containerId2,
      containerConfig,
      std::map<string, string>(),
      None());

  AWAIT_FAILED(launch);
}


// This test verifies that we do not support launching debug container
// with private IPC mode.
TEST_F(NamespacesIsolatorTest, ROOT_DebugContainerWithPrivateIPCMode)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("filesystem/linux,namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Launch a top-level container.
  process::Future<Containerizer::LaunchResult> launch =
    containerizer.get()->launch(
        containerId,
        createContainerConfig(
              None(),
              createExecutorInfo("executor", "sleep 1000"),
              directory),
        std::map<string, string>(),
        None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Launch a debug container with private IPC mode under the
  // top-level container.
  ContainerID debugContainerId;
  debugContainerId.mutable_parent()->CopyFrom(containerId);
  debugContainerId.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_linux_info()->set_ipc_mode(LinuxInfo::PRIVATE);

  launch = containerizer.get()->launch(
      debugContainerId,
      createContainerConfig(
          createCommandInfo("sleep 1000"),
          containerInfo,
          ContainerClass::DEBUG),
      std::map<string, string>(),
      None());

  AWAIT_FAILED(launch);

  Future<Option<ContainerTermination>> termination =
    containerizer.get()->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
