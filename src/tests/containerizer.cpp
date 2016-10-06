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

#include "tests/containerizer.hpp"

#include <mutex>

#include "stout/synchronized.hpp"

#include "tests/mesos.hpp"

using namespace process;

using std::map;
using std::shared_ptr;
using std::string;

using testing::_;
using testing::Invoke;
using testing::Return;

using mesos::slave::ContainerTermination;

using mesos::v1::executor::Mesos;

namespace mesos {
namespace internal {
namespace tests {

TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    const shared_ptr<MockV1HTTPExecutor>& executor)
{
  executors[executorId] = Owned<ExecutorData>(new ExecutorData());
  executors.at(executorId)->v1ExecutorMock = executor;

  setup();
}


TestContainerizer::TestContainerizer(
    const hashmap<ExecutorID, Executor*>& _executors)
{
  foreachpair (const ExecutorID& executorId, Executor* executor, _executors) {
    executors[executorId] = Owned<ExecutorData>(new ExecutorData());
    executors.at(executorId)->executor = executor;
  }

  setup();
}


TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    Executor* executor)
{
  executors[executorId] = Owned<ExecutorData>(new ExecutorData());
  executors.at(executorId)->executor = executor;

  setup();
}


TestContainerizer::TestContainerizer(MockExecutor* executor)
{
  executors[executor->id] = Owned<ExecutorData>(new ExecutorData());
  executors.at(executor->id)->executor = executor;

  setup();
}


TestContainerizer::TestContainerizer()
{
  setup();
}


TestContainerizer::~TestContainerizer()
{
  foreachvalue (const Owned<ExecutorData>& data, executors) {
    if (data->driver.get() != nullptr) {
      data->driver->stop();
      data->driver->join();
    }
  }
}


Future<bool> TestContainerizer::_launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint)
{
  CHECK(!terminatedContainers.contains(containerId))
    << "Failed to launch nested container " << containerId
    << " for executor '" << executorInfo.executor_id() << "'"
    << " of framework " << executorInfo.framework_id()
    << " because this ContainerID is being re-used with"
    << " a previously terminated container";

  CHECK(!containers_.contains(containerId))
    << "Failed to launch container " << containerId
    << " for executor '" << executorInfo.executor_id() << "'"
    << " of framework " << executorInfo.framework_id()
    << " because it is already launched";

  CHECK(executors.contains(executorInfo.executor_id()))
    << "Failed to launch executor '" << executorInfo.executor_id() << "'"
    << " of framework " << executorInfo.framework_id()
    << " because it is unknown to the containerizer";

  containers_[containerId] = Owned<ContainerData>(new ContainerData());
  containers_.at(containerId)->executorId = executorInfo.executor_id();
  containers_.at(containerId)->frameworkId = executorInfo.framework_id();

  // We need to synchronize all reads and writes to the environment as this is
  // global state.
  // TODO(jmlvanre): Even this is not sufficient, as other aspects of the code
  // may read an environment variable while we are manipulating it. The better
  // solution is to pass the environment variables into the fork, or to set them
  // on the command line. See MESOS-3475.
  static std::mutex mutex;

  synchronized(mutex) {
    // Since the constructor for `MesosExecutorDriver` reads environment
    // variables to load flags, even it needs to be within this synchronization
    // section.
    // Prepare additional environment variables for the executor.
    // TODO(benh): Need to get flags passed into the TestContainerizer
    // in order to properly use here.
    slave::Flags flags;
    flags.recovery_timeout = Duration::zero();

    // We need to save the original set of environment variables so we
    // can reset the environment after calling 'driver->start()' below.
    hashmap<string, string> original = os::environment();

    foreachpair (const string& name, const string variable, environment) {
      os::setenv(name, variable);
    }

    // TODO(benh): Can this be removed and done exlusively in the
    // 'executorEnvironment()' function? There are other places in the
    // code where we do this as well and it's likely we can do this once
    // in 'executorEnvironment()'.
    foreach (const Environment::Variable& variable,
             executorInfo.command().environment().variables()) {
      os::setenv(variable.name(), variable.value());
    }

    os::setenv("MESOS_LOCAL", "1");

    const Owned<ExecutorData>& executorData =
      executors.at(executorInfo.executor_id());

    if (executorData->executor != nullptr) {
      executorData->driver = Owned<MesosExecutorDriver>(
          new MesosExecutorDriver(executorData->executor));
      executorData->driver->start();
    } else {
      shared_ptr<MockV1HTTPExecutor> executor = executorData->v1ExecutorMock;
      executorData->v1Library = Owned<executor::TestV1Mesos>(
        new executor::TestV1Mesos(ContentType::PROTOBUF, executor));
    }

    os::unsetenv("MESOS_LOCAL");

    // Unset the environment variables we set by resetting them to their
    // original values and also removing any that were not part of the
    // original environment.
    foreachpair (const string& name, const string& value, original) {
      os::setenv(name, value);
    }

    foreachkey (const string& name, environment) {
      if (!original.contains(name)) {
        os::unsetenv(name);
      }
    }
  }

  return true;
}


Future<bool> TestContainerizer::_launch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo,
    const Option<string>& user,
    const SlaveID& slaveId)
{
  CHECK(!terminatedContainers.contains(containerId))
    << "Failed to launch nested container " << containerId
    << " because this ContainerID is being re-used with"
    << " a previously terminated container";

  CHECK(!containers_.contains(containerId))
    << "Failed to launch nested container " << containerId
    << " because it is already launched";

  containers_[containerId] = Owned<ContainerData>(new ContainerData());

  // No-op for now.
  return true;
}


Future<Option<ContainerTermination>> TestContainerizer::_wait(
    const ContainerID& containerId) const
{
  if (terminatedContainers.contains(containerId)) {
    return terminatedContainers.at(containerId);
  }

  // An unknown container is possible for tests where we "drop" the
  // 'launch' in order to verify recovery still works correctly.
  if (!containers_.contains(containerId)) {
    return None();
  }

  return containers_.at(containerId)->termination.future()
    .then(Option<ContainerTermination>::some);
}


Future<bool> TestContainerizer::destroy(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  Option<ContainerID> containerId = None();

  foreachpair (const ContainerID& containerId_,
               const Owned<ContainerData>& container,
               containers_) {
    if (container->frameworkId == frameworkId &&
        container->executorId == executorId) {
      containerId = containerId_;
    }
  }

  if (containerId.isNone()) {
    LOG(WARNING) << "Ignoring destroy of unknown container"
                 << " for executor '" << executorId << "'"
                 << " of framework " << frameworkId;
    return false;
  }

  return _destroy(containerId.get());
}


Future<bool> TestContainerizer::_destroy(const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return false;
  }

  const Owned<ContainerData>& containerData = containers_.at(containerId);

  if (containerData->executorId.isSome()) {
    CHECK(executors.contains(containerData->executorId.get()));

    const Owned<ExecutorData>& executorData =
      executors.at(containerData->executorId.get());

    if (executorData->driver.get() != nullptr) {
      executorData->driver->stop();
      executorData->driver->join();
    }

    executors.erase(containerData->executorId.get());
  }

  ContainerTermination termination;
  termination.set_message("Killed executor");
  termination.set_status(0);

  containerData->termination.set(termination);

  containers_.erase(containerId);
  terminatedContainers[containerId] = termination;

  return true;
}


Future<hashset<ContainerID>> TestContainerizer::containers()
{
  return containers_.keys();
}


void TestContainerizer::setup()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault' because the latter gives the gmock
  // warning "Uninteresting mock function call" unless each tests puts
  // the expectations in place which would make the tests much more
  // verbose.
  //
  // TODO(bmahler): Update this to use the same style as the
  // TestAllocator, which allows us to have default actions
  // 'DoDefault', without requiring each test to put expectations in
  // place.

  EXPECT_CALL(*this, recover(_))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, usage(_))
    .WillRepeatedly(Return(ResourceStatistics()));

  EXPECT_CALL(*this, status(_))
    .WillRepeatedly(Return(ContainerStatus()));

  EXPECT_CALL(*this, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  Future<bool> (TestContainerizer::*_launch)(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const map<string, string>& environment,
      bool checkpoint) =
    &TestContainerizer::_launch;

  EXPECT_CALL(*this, launch(_, _, _, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, _launch));

  Future<bool> (TestContainerizer::*_launchNested)(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<ContainerInfo>& containerInfo,
      const Option<string>& user,
      const SlaveID& slaveId) =
    &TestContainerizer::_launch;

  EXPECT_CALL(*this, launch(_, _, _, _, _))
    .WillRepeatedly(Invoke(this, _launchNested));

  EXPECT_CALL(*this, wait(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_wait));

  EXPECT_CALL(*this, destroy(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_destroy));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
