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

#include "tests/mesos.hpp"

using std::string;

using process::Future;
using process::Owned;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

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
      const Option<bool>& disallowSharingAgentPidNamespace = None())
  {
    slave::Flags flags = CreateSlaveFlags();
    flags.isolation = isolation;

    if (disallowSharingAgentPidNamespace.isSome()) {
      flags.disallow_sharing_agent_pid_namespace =
        disallowSharingAgentPidNamespace.get();
    }

    fetcher.reset(new Fetcher(flags));

    Try<MesosContainerizer*> _containerizer =
      MesosContainerizer::create(flags, false, fetcher.get());

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


// The IPC namespace has its own copy of the svipc(7) tunables. We verify
// that we are correctly entering the IPC namespace by verifying that we
// can set shmmax some different value than that of the host namespace.
TEST_F(NamespacesIsolatorTest, ROOT_IPCNamespace)
{
  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer("namespaces/ipc");

  ASSERT_SOME(containerizer);

  // Value we will set the child namespace shmmax to.
  uint64_t shmmaxValue = static_cast<uint64_t>(::getpid());

  Try<uint64_t> hostShmmax = readValue("/proc/sys/kernel/shmmax");
  ASSERT_SOME(hostShmmax);

  // Verify that the host namespace shmmax is different.
  ASSERT_NE(hostShmmax.get(), shmmaxValue);

  const string command =
    "stat -Lc %i /proc/self/ns/ipc > ns;"
    "echo " + stringify(shmmaxValue) + " > /proc/sys/kernel/shmmax;"
    "cp /proc/sys/kernel/shmmax shmmax";

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

  // Check that the command was run in a different IPC namespace.
  Result<ino_t> testIPCNamespace = ns::getns(::getpid(), "ipc");
  ASSERT_SOME(testIPCNamespace);

  Try<string> containerIPCNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerIPCNamespace);

  EXPECT_NE(stringify(testIPCNamespace.get()),
            strings::trim(containerIPCNamespace.get()));

  // Check that we modified the IPC shmmax of the namespace, not the host.
  Try<uint64_t> childShmmax = readValue("shmmax");
  ASSERT_SOME(childShmmax);

  // Verify that we didn't modify shmmax in the host namespace.
  ASSERT_EQ(hostShmmax.get(), readValue("/proc/sys/kernel/shmmax").get());

  EXPECT_NE(hostShmmax.get(), childShmmax.get());
  EXPECT_EQ(shmmaxValue, childShmmax.get());
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
