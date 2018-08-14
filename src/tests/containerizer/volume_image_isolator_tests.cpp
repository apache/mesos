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

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/cluster.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::slave::ContainerTermination;

using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

class VolumeImageIsolatorTest :
  public MesosTest,
  public ::testing::WithParamInterface<bool>
{
protected:
  void SetUp() override
  {
    nesting = GetParam();

    MesosTest::SetUp();
  }

  bool nesting;
};


INSTANTIATE_TEST_CASE_P(
    Nesting,
    VolumeImageIsolatorTest,
    ::testing::Values(false, true));


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is not specified.
TEST_P(VolumeImageIsolatorTest, ROOT_ImageInVolumeWithoutRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/image,docker/runtime";
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

  ContainerInfo container = createContainerInfo(
      None(),
      {createVolumeFromDockerImage("rootfs", "test_image", Volume::RW)});

  CommandInfo command = createCommandInfo("test -d rootfs/bin");

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      nesting ? createCommandInfo("sleep 1000") : command);

  if (!nesting) {
    executor.mutable_container()->CopyFrom(container);
  }

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  if (nesting) {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    launch = containerizer->launch(
        nestedContainerId,
        createContainerConfig(command, container),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

    wait = containerizer->wait(nestedContainerId);
  }

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  if (nesting) {
    Future<Option<ContainerTermination>> termination =
      containerizer->destroy(containerId);

    AWAIT_READY(termination);
    ASSERT_SOME(termination.get());
    ASSERT_TRUE(termination->get().has_status());
    EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
  }
}


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is specified.
TEST_P(VolumeImageIsolatorTest, ROOT_ImageInVolumeWithRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image_rootfs"));
  AWAIT_READY(DockerArchive::create(registry, "test_image_volume"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/image,docker/runtime";
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

  ContainerInfo container = createContainerInfo(
      "test_image_rootfs",
      {createVolumeFromDockerImage(
          "rootfs", "test_image_volume", Volume::RW)});

  CommandInfo command = createCommandInfo(
      "[ ! -d '" + sandbox.get() + "' ] && [ -d rootfs/bin ]");

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      nesting ? createCommandInfo("sleep 1000") : command);

  if (!nesting) {
    executor.mutable_container()->CopyFrom(container);
  }

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  if (nesting) {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    launch = containerizer->launch(
        nestedContainerId,
        createContainerConfig(command, container),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

    wait = containerizer->wait(nestedContainerId);
  }

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());

  if (nesting) {
    Future<Option<ContainerTermination>> termination =
      containerizer->destroy(containerId);

    AWAIT_READY(termination);
    ASSERT_SOME(termination.get());
    ASSERT_TRUE(termination->get().has_status());
    EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
  }
}


// This test verifies that a container launched without
// a rootfs cannot write to a read-only IMAGE volume.
TEST_P(VolumeImageIsolatorTest, ROOT_ImageInReadOnlyVolumeWithoutRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/image,docker/runtime";
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

  ContainerInfo container = createContainerInfo(
      None(),
      {createVolumeFromDockerImage("rootfs", "test_image", Volume::RO)});

  CommandInfo command = createCommandInfo("echo abc > rootfs/file");

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      nesting ? createCommandInfo("sleep 1000") : command);

  if (!nesting) {
    executor.mutable_container()->CopyFrom(container);
  }

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  if (nesting) {
    ContainerID nestedContainerId;
    nestedContainerId.mutable_parent()->CopyFrom(containerId);
    nestedContainerId.set_value(id::UUID::random().toString());

    launch = containerizer->launch(
        nestedContainerId,
        createContainerConfig(command, container),
        map<string, string>(),
        None());

    AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

    wait = containerizer->wait(nestedContainerId);
  }

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_NE(0, wait->get().status());

  if (nesting) {
    Future<Option<ContainerTermination>> termination =
      containerizer->destroy(containerId);

    AWAIT_READY(termination);
    ASSERT_SOME(termination.get());
    ASSERT_TRUE(termination->get().has_status());
    EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
