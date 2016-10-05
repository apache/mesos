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

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class VolumeImageIsolatorTest : public MesosTest
{
protected:
  Fetcher fetcher;
};


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is not specified.
TEST_F(VolumeImageIsolatorTest, ROOT_ImageInVolumeWithoutRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/image,docker/runtime";
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
      "test -d rootfs/bin");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      None(),
      {createVolumeFromDockerImage("rootfs", "test_image", Volume::RW)}));

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


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is specified.
TEST_F(VolumeImageIsolatorTest, ROOT_ImageInVolumeWithRootFilesystem)
{
  string registry = path::join(sandbox.get(), "registry");
  AWAIT_READY(DockerArchive::create(registry, "test_image_rootfs"));
  AWAIT_READY(DockerArchive::create(registry, "test_image_volume"));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/image,docker/runtime";
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
      "[ ! -d '" + sandbox.get() + "' ] && [ -d rootfs/bin ]");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image_rootfs",
      {createVolumeFromDockerImage(
          "rootfs", "test_image_volume", Volume::RW)}));

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
