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

#include <mesos/mesos.hpp>

#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/try.hpp>

#include "common/kernel_version.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;

using std::map;
using std::string;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

class LinuxNNPIsolatorTest : public MesosTest
{
protected:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();
    flags.isolation = "filesystem/linux,docker/runtime,linux/nnp";
    flags.docker_registry = GetRegistryPath();
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
    flags.launcher = "linux";

    return flags;
  }

  string GetRegistryPath() const
  {
    return path::join(sandbox.get(), "registry");
  }
};


// Check that the PR_NO_NEW_PRIVILEGES flag is set.
TEST_F(LinuxNNPIsolatorTest, ROOT_CheckNoNewPrivileges)
{
  // This tests requires the NoNewPrivs field present in process
  // status fields which requires Linux kernel version greater than
  // or equal to 4.10.
  Try<Version> version = mesos::kernelVersion();
  ASSERT_SOME(version);
  if (version.get() < Version(4, 10, 0)) {
    LOG(INFO) << "Linux kernel version greater than or equal to 4.10 required";
    return;
  }

  AWAIT_READY(DockerArchive::create(GetRegistryPath(), "test_image"));

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Test that the child process inherits the PR_NO_NEW_PRIVS flag.
  // Using convoluted way to parse the process status file
  // due to minimal docker image. The child process should inherit
  // the PR_NO_NEW_PRIVS flag. Parse the process status file and
  // determine if "NoNewPrivs: 1" is found.
  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      R"~(
      nnp_seen="false"
      for word in $(cat /proc/self/status); do
        if [ "$word" = "NoNewPrivs:" ]; then
          nnp_seen="true"
        elif [ "$nnp_seen" = "true" ]; then
          if [ "$word" = "1" ]; then
            exit 0
          else
            exit 1
          fi
        fi
      done
      exit 1
      )~");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
