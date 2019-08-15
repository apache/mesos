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
#include <set>
#include <string>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>

#include "slave/containerizer/mesos/paths.hpp"

#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::internal::slave::containerizer::paths::SECRET_DIRECTORY;

using mesos::slave::ContainerTermination;

using std::list;
using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

const char SECRET_VALUE[] = "password";


enum FS_TYPE {
  WITH_ROOTFS,
  WITHOUT_ROOTFS
};


enum CONTAINER_LAUNCH_STATUS {
  CONTAINER_LAUNCH_FAILURE,
  CONTAINER_LAUNCH_SUCCESS
};


class VolumeSecretIsolatorTest :
  public MesosTest,
  public ::testing::WithParamInterface<std::tr1::tuple<
      const char*,
      const char*,
      enum FS_TYPE,
      enum CONTAINER_LAUNCH_STATUS,
      Volume::Mode>>

{
protected:
  void SetUp() override
  {
    const char* prefix = std::tr1::get<0>(GetParam());
    const char* path = std::tr1::get<1>(GetParam());
    secretContainerPath = string(prefix) + string(path);

    fsType = std::tr1::get<2>(GetParam());
    expectedContainerLaunchStatus = std::tr1::get<3>(GetParam());

    volume.set_mode(std::tr1::get<4>(GetParam()));
    volume.set_container_path(secretContainerPath);

    Volume::Source* source = volume.mutable_source();
    source->set_type(Volume::Source::SECRET);

    // Request a secret.
    Secret* secret = source->mutable_secret();
    secret->set_type(Secret::VALUE);
    secret->mutable_value()->set_data(SECRET_VALUE);

    MesosTest::SetUp();
  }

  string secretContainerPath;
  bool expectedContainerLaunchStatus;
  bool fsType;

  Volume volume;
};


static const char* paths[] = {
  "my_secret",
  "some/my_secret",
  "etc/my_secret",
  "etc/some/my_secret"
};


INSTANTIATE_TEST_CASE_P(
    SecretTestTypeWithoutRootFSRelativePath,
    VolumeSecretIsolatorTest,
    ::testing::Combine(::testing::Values(""),
                       ::testing::ValuesIn(paths),
                       ::testing::Values(WITHOUT_ROOTFS),
                       ::testing::Values(CONTAINER_LAUNCH_SUCCESS),
                       ::testing::Values(Volume::RW, Volume::RO)));


INSTANTIATE_TEST_CASE_P(
    SecretTestTypeWithoutRootFSNonExisitingAbsolutePath,
    VolumeSecretIsolatorTest,
    ::testing::Combine(::testing::Values("/"),
                       ::testing::ValuesIn(paths),
                       ::testing::Values(WITHOUT_ROOTFS),
                       ::testing::Values(CONTAINER_LAUNCH_FAILURE),
                       ::testing::Values(Volume::RW, Volume::RO)));


INSTANTIATE_TEST_CASE_P(
    SecretTestTypeWithoutRootFSExistingAbsolutePath,
    VolumeSecretIsolatorTest,
    ::testing::Combine(::testing::Values(""),
                       ::testing::Values("/bin/touch"),
                       ::testing::Values(WITHOUT_ROOTFS),
                       ::testing::Values(CONTAINER_LAUNCH_SUCCESS),
                       ::testing::Values(Volume::RW, Volume::RO)));


INSTANTIATE_TEST_CASE_P(
    SecretTestTypeWithRootFS,
    VolumeSecretIsolatorTest,
    ::testing::Combine(::testing::Values("", "/"),
                       ::testing::ValuesIn(paths),
                       ::testing::Values(WITH_ROOTFS),
                       ::testing::Values(CONTAINER_LAUNCH_SUCCESS),
                       ::testing::Values(Volume::RW, Volume::RO)));


TEST_P(VolumeSecretIsolatorTest, ROOT_SecretInVolumeWithRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/secret";

  if (fsType == WITH_ROOTFS) {
    const string registry = path::join(sandbox.get(), "registry");
    AWAIT_READY(DockerArchive::create(registry, "test_image_rootfs"));
    AWAIT_READY(DockerArchive::create(registry, "test_image_volume"));

    flags.isolation += ",volume/image,docker/runtime";
    flags.docker_registry = registry;
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
  }

  Fetcher fetcher(flags);

  Try<SecretResolver*> secretResolver = SecretResolver::create();
  EXPECT_SOME(secretResolver);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      secretResolver.get());

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  if (fsType == WITH_ROOTFS) {
    containerInfo = createContainerInfo(
        "test_image_rootfs",
        {createVolumeFromDockerImage(
            "rootfs", "test_image_volume", Volume::RW)});
  } else {
    containerInfo.set_type(ContainerInfo::MESOS);
  }

  containerInfo.add_volumes()->CopyFrom(volume);

  CommandInfo command = createCommandInfo(
      "secret=$(cat " + secretContainerPath + "); "
      "test \"$secret\" = \"" + string(SECRET_VALUE) + "\" && sleep 1000");

  ExecutorInfo executor = createExecutorInfo("test_executor", command);
  executor.mutable_container()->CopyFrom(containerInfo);

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  if (expectedContainerLaunchStatus == CONTAINER_LAUNCH_FAILURE) {
    AWAIT_FAILED(launch);
    return;
  }

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  CommandInfo nestedCommand = createCommandInfo(
      volume.mode() == Volume::RW
        ? "secret=$(cat " + secretContainerPath + "); "
          "test \"$secret\" = \"" + string(SECRET_VALUE) + "\""
        : "echo abc > " + secretContainerPath);

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(nestedCommand, containerInfo),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  // Wait for nested container.
  Future<Option<ContainerTermination>> wait = containerizer->wait(
      nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());

  if (volume.mode() == Volume::RW) {
    EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());
  } else {
    EXPECT_WEXITSTATUS_NE(0, wait.get()->status());
  }

  // Now wait for parent container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);
  ASSERT_SOME(termination.get());
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, termination.get()->status());
}


class VolumeSecretIsolatorCleanupTest : public MesosTest {};


// This test verifies that container directory created by `volume/secret`
// isolator can be cleaned up when the container is destroyed.
TEST_F(VolumeSecretIsolatorCleanupTest, ROOT_FailInPreparing)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,volume/secret,network/cni";

  Fetcher fetcher(flags);

  Try<SecretResolver*> secretResolver = SecretResolver::create();
  EXPECT_SOME(secretResolver);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      secretResolver.get());

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  Volume volume;
  volume.set_mode(Volume::RW);
  volume.set_container_path("my_secret");

  Volume::Source* source = volume.mutable_source();
  source->set_type(Volume::Source::SECRET);

  // Request a secret.
  Secret* secret = source->mutable_secret();
  secret->set_type(Secret::VALUE);
  secret->mutable_value()->set_data(SECRET_VALUE);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume);

  // Specify a nonexistent CNI network to make container fails to launch.
  NetworkInfo* networkInfo = containerInfo.add_network_infos();
  networkInfo->set_name("nonexistent_network");

  ExecutorInfo executor = createExecutorInfo("test_executor", "sleep 1000");
  executor.mutable_container()->CopyFrom(containerInfo);

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory),
      map<string, string>(),
      None());

  AWAIT_FAILED(launch);

  // Check the container directory is created.
  const string containerDir = path::join(
      flags.runtime_dir,
      SECRET_DIRECTORY,
      stringify(containerId));

  ASSERT_TRUE(os::exists(containerDir));

  // Check there is one secret resolved and written to the container directory.
  Try<list<string>> secretFiles = os::ls(containerDir);
  ASSERT_SOME(secretFiles);
  ASSERT_EQ(secretFiles->size(), 1u);

  // Destroy the container.
  Future<Option<ContainerTermination>> termination =
    containerizer->destroy(containerId);

  AWAIT_READY(termination);

  // Check the container directory is removed.
  ASSERT_FALSE(os::exists(containerDir));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
