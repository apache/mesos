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

#include <string>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class NestedContainerTest : public ContainerizerTest<MesosContainerizer> {};


TEST_F(NestedContainerTest, ROOT_CGROUPS_WaitAfterDestroy)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  Fetcher fetcher;

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveID slaveId = SlaveID();

  // Launch a top-level container.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "sleep 1000");
  executor.mutable_resources()->CopyFrom(Resources::parse("cpus:1").get());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      slaveId,
      map<string, string>(),
      true);

  AWAIT_ASSERT_TRUE(launch);

  // Launch a nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("exit 42"),
      None(),
      None(),
      slaveId);

  AWAIT_ASSERT_TRUE(launch);

  // Wait once (which does a destroy),
  // then wait again on the nested container.
  Future<Option<ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  nestedWait = containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());
  ASSERT_TRUE(nestedWait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(42, nestedWait.get()->status());

  // Destroy the top-level container.
  Future<Option<ContainerTermination>> wait =
    containerizer->wait(containerId);

  AWAIT_READY(containerizer->destroy(containerId));

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  // Wait on nested container again.
  nestedWait = containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_NONE(nestedWait.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
