/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using std::map;
using std::string;

using namespace process;

namespace mesos {
namespace internal {
namespace tests {


TestContainerizer::TestContainerizer(
    const hashmap<ExecutorID, Executor*>& _executors)
  : executors(_executors)
{
  setup();
}


TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    Executor* executor)
{
  executors[executorId] = executor;
  setup();
}


TestContainerizer::TestContainerizer(MockExecutor* executor)
{
  executors[executor->id] = executor;
  setup();
}


TestContainerizer::TestContainerizer()
{
  setup();
}


TestContainerizer::~TestContainerizer()
{
  foreachvalue (const Owned<MesosExecutorDriver>& driver, drivers) {
    driver->stop();
    driver->join();
  }
  drivers.clear();
}


Future<Nothing> TestContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<slave::Slave>& slavePid,
    bool checkpoint)
{
  CHECK(!drivers.contains(containerId))
    << "Failed to launch executor " << executorInfo.executor_id()
    << " of framework " << executorInfo.framework_id()
    << " because it is already launched";

  CHECK(executors.contains(executorInfo.executor_id()))
    << "Failed to launch executor " << executorInfo.executor_id()
    << " of framework " << executorInfo.framework_id()
    << " because it is unknown to the containerizer";

  // Store mapping from (frameworkId, executorId) -> containerId to facilitate
  // easy destroy from tests.
  std::pair<FrameworkID, ExecutorID> key(executorInfo.framework_id(),
                                         executorInfo.executor_id());
  containers[key] = containerId;

  Executor* executor = executors[executorInfo.executor_id()];
  Owned<MesosExecutorDriver> driver(new MesosExecutorDriver(executor));
  drivers[containerId] = driver;

  // Prepare additional environment variables for the executor.
  const map<string, string>& env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      Duration::zero());

  foreachpair (const string& name, const string variable, env) {
    os::setenv(name, variable);
  }

  foreach (const Environment_Variable& variable,
      executorInfo.command().environment().variables()) {
    os::setenv(variable.name(), variable.value());
  }
  os::setenv("MESOS_LOCAL", "1");

  driver->start();

  foreachkey (const string& name, env) {
    os::unsetenv(name);
  }

  foreach(const Environment_Variable& variable,
      executorInfo.command().environment().variables()) {
    os::unsetenv(variable.name());
  }
  os::unsetenv("MESOS_LOCAL");

  Owned<Promise<slave::Containerizer::Termination> > promise(
      new Promise<slave::Containerizer::Termination>());
  promises[containerId] = promise;

  return Nothing();
}


Future<slave::Containerizer::Termination> TestContainerizer::wait(
    const ContainerID& containerId)
{
  CHECK(promises.contains(containerId))
    << "Container " << containerId << "not started";

  return promises[containerId]->future();
}


void TestContainerizer::destroy(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  std::pair<FrameworkID, ExecutorID> key(frameworkId, executorId);
  if (!containers.contains(key)) {
    LOG(WARNING) << "Ignoring destroy of unknown container for executor '"
                  << executorId << "' of framework '" << frameworkId << "'";
    return;
  }
  destroy(containers[key]);
}


void TestContainerizer::destroy(const ContainerID& containerId)
{
  CHECK(drivers.contains(containerId))
    << "Failed to terminate container " << containerId
    << " because it is has not been started";

  Owned<MesosExecutorDriver> driver = drivers[containerId];
  driver->stop();
  driver->join();
  drivers.erase(containerId);

  promises[containerId]->set(
      slave::Containerizer::Termination(0, false, "Killed executor"));
  promises.erase(containerId);
}


void TestContainerizer::setup()
{
  EXPECT_CALL(*this, recover(testing::_))
    .WillRepeatedly(testing::Return(Nothing()));

  EXPECT_CALL(*this, usage(testing::_))
    .WillRepeatedly(testing::Return(ResourceStatistics()));

  EXPECT_CALL(*this, update(testing::_, testing::_))
    .WillRepeatedly(testing::Return(Nothing()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
