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

#include <map>
#include <string>

#include <stout/gtest.hpp>
#include <stout/path.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#ifdef __linux__
#include "tests/containerizer/docker_archive.hpp"
#endif

using std::map;
using std::string;

using process::Future;
using process::Owned;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

class VolumeSandboxPathIsolatorTest : public MesosTest {};


#ifdef __linux__
// This test verifies that a SANDBOX_PATH volume with SELF type is
// properly created in the container's sandbox and is properly mounted
// in the container's mount namespace.
TEST_F(VolumeSandboxPathIsolatorTest, ROOT_SelfType)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/sandbox_path,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeSandboxPath("/tmp", "tmp", Volume::RW)}));

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

  EXPECT_SOME_EQ("abc\n", os::read(path::join(directory, "tmp", "file")));
}


// This test verifies that a container launched with a rootfs cannot
// write to a read-only SANDBOX_PATH volume with SELF type.
TEST_F(VolumeSandboxPathIsolatorTest, ROOT_SelfTypeReadOnly)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/sandbox_path,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeSandboxPath("/tmp", "tmp", Volume::RO)}));

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
#endif // __linux__


// This test verifies that sandbox path volume allows two containers
// nested under the same parent container to share data.
// TODO(jieyu): Parameterize this test to test both linux and posix
// launcher and filesystem isolator.
TEST_F(VolumeSandboxPathIsolatorTest, SharedParentTypeVolume)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "volume/sandbox_path";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo("executor", "sleep 99", "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);

  Volume* volume = containerInfo.add_volumes();
  volume->set_mode(Volume::RW);
  volume->set_container_path("parent");

  Volume::Source* source = volume->mutable_source();
  source->set_type(Volume::Source::SANDBOX_PATH);

  Volume::Source::SandboxPath* sandboxPath = source->mutable_sandbox_path();
  sandboxPath->set_type(Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("shared");

  launch = containerizer->launch(
      nestedContainerId1,
      createContainerConfig(
          createCommandInfo("touch parent/file; sleep 1000"),
          containerInfo),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(id::UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId2,
      createContainerConfig(
          createCommandInfo(
            "while true; do if [ -f parent/file ]; then exit 0; fi; done"),
          containerInfo),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


#ifdef __linux__
// This is a regression test for MESOS-5187. It is a ROOT test to
// simulate the scenario that the framework user is non-root while
// the agent process is root, to make sure that non-root user can
// still have the permission to write to the volume as expected.
TEST_F(VolumeSandboxPathIsolatorTest,
       ROOT_UNPRIVILEGED_USER_SelfTypeOwnership)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/sandbox_path,docker/runtime";
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeSandboxPath("/tmp", "tmp", Volume::RW)}));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  // Simulate the executor sandbox ownership as the user
  // from FrameworkInfo.
  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  ASSERT_SOME(os::chown(user.get(), directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory, user.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(directory, "tmp", "file")));
}
#endif // __linux__


// This is a regression test for MESOS-7830. It is a ROOT test to
// simulate the scenario that the framework user is non-root while
// the agent process is root, to make sure that non-root user can
// still have the permission to write to the volume as expected.
TEST_F(VolumeSandboxPathIsolatorTest,
       ROOT_UNPRIVILEGED_USER_ParentTypeOwnership)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "volume/sandbox_path";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo("executor", "sleep 99", "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Simulate the executor sandbox ownership as the user
  // from FrameworkInfo.
  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  ASSERT_SOME(os::chown(user.get(), directory.get()));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get(), user.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);

  Volume* volume = containerInfo.add_volumes();
  volume->set_mode(Volume::RW);
  volume->set_container_path("parent");

  Volume::Source* source = volume->mutable_source();
  source->set_type(Volume::Source::SANDBOX_PATH);

  Volume::Source::SandboxPath* sandboxPath = source->mutable_sandbox_path();
  sandboxPath->set_type(Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("shared");

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo("echo 'hello' > parent/file"),
          containerInfo,
          None(),
          user.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
