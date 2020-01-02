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

using process::Failure;
using process::Future;
using process::Owned;

using process::http::Connection;

using std::map;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;
using testing::Invoke;
using testing::Return;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerTermination;

using mesos::v1::executor::Mesos;

namespace mesos {
namespace internal {
namespace tests {


class TestContainerizerProcess
  : public process::Process<TestContainerizerProcess>
{
public:
  TestContainerizerProcess() {}

  TestContainerizerProcess(
      const ExecutorID& executorId,
      const std::shared_ptr<v1::MockHTTPExecutor>& executor)
  {
    executors[executorId] = Owned<ExecutorData>(new ExecutorData());
    executors.at(executorId)->v1ExecutorMock = executor;
  }

  TestContainerizerProcess(
      const hashmap<ExecutorID, Executor*>& _executors)
  {
    foreachpair (const ExecutorID& executorId, Executor* executor, _executors) {
      executors[executorId] = Owned<ExecutorData>(new ExecutorData());
      executors.at(executorId)->executor = executor;
    }
  }

  ~TestContainerizerProcess() override
  {
    foreachvalue (const Owned<ExecutorData>& data, executors) {
      if (data->driver.get() != nullptr) {
        data->driver->stop();
        data->driver->join();
      }
    }
  }

  Future<Nothing> recover(
      const Option<slave::state::SlaveState>& state)
  {
    return Nothing();
  }

  Future<slave::Containerizer::LaunchResult> launch(
      const ContainerID& containerId,
      const ContainerConfig& containerConfig,
      const map<string, string>& environment,
      const Option<string>& pidCheckpointPath)
  {
    CHECK(!terminatedContainers.contains(containerId))
      << "Failed to launch nested container " << containerId
      << " for executor '" << containerConfig.executor_info().executor_id()
      << "' of framework " << containerConfig.executor_info().framework_id()
      << " because this ContainerID is being re-used with"
      << " a previously terminated container";

    CHECK(!containers_.contains(containerId))
      << "Failed to launch container " << containerId
      << " for executor '" << containerConfig.executor_info().executor_id()
      << "' of framework " << containerConfig.executor_info().framework_id()
      << " because it is already launched";

    containers_[containerId] = Owned<ContainerData>(new ContainerData());

    if (containerId.has_parent()) {
      // Launching a nested container via the test containerizer is a
      // no-op for now.
      return slave::Containerizer::LaunchResult::SUCCESS;
    }

    CHECK(executors.contains(containerConfig.executor_info().executor_id()))
      << "Failed to launch executor '"
      << containerConfig.executor_info().executor_id()
      << "' of framework " << containerConfig.executor_info().framework_id()
      << " because it is unknown to the containerizer";

    containers_.at(containerId)->executorId =
      containerConfig.executor_info().executor_id();

    containers_.at(containerId)->frameworkId =
      containerConfig.executor_info().framework_id();

    // Assemble the environment for the executor.
    //
    // NOTE: Since in this case the executor will live in the same OS process,
    // pass the environment into the executor driver (library) c-tor directly
    // instead of manipulating `setenv`/`getenv` to avoid concurrent
    // modification of the environment.
    map<string, string> fullEnvironment = os::environment();

    fullEnvironment.insert(environment.begin(), environment.end());

    // TODO(benh): Can this be removed and done exclusively in the
    // 'executorEnvironment()' function? There are other places in the
    // code where we do this as well and it's likely we can do this once
    // in 'executorEnvironment()'.
    foreach (const Environment::Variable& variable,
             containerConfig.executor_info()
               .command().environment().variables()) {
      fullEnvironment.emplace(variable.name(), variable.value());
    }

    fullEnvironment.emplace("MESOS_LOCAL", "1");

    const Owned<ExecutorData>& executorData =
      executors.at(containerConfig.executor_info().executor_id());

    if (executorData->executor != nullptr) {
      executorData->driver = Owned<MesosExecutorDriver>(
          new MesosExecutorDriver(executorData->executor, fullEnvironment));
      executorData->driver->start();
    } else {
      shared_ptr<v1::MockHTTPExecutor> executor =
        executorData->v1ExecutorMock;
      executorData->v1Library = Owned<v1::executor::TestMesos>(
          new v1::executor::TestMesos(
              ContentType::PROTOBUF, executor, fullEnvironment));
    }

    // Checkpoint the forked pid if requested by the agent.
    if (pidCheckpointPath.isSome()) {
      Try<Nothing> checkpointed = slave::state::checkpoint(
          pidCheckpointPath.get(), stringify(::getpid()));

      if (checkpointed.isError()) {
        LOG(ERROR) << "Failed to checkpoint container's forked pid to '"
                   << pidCheckpointPath.get() << "': " << checkpointed.error();
        return Failure("Could not checkpoint container's pid");
      }
    }

    return slave::Containerizer::LaunchResult::SUCCESS;
  }

  Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
  {
    return Nothing();
  }

  Future<Connection> attach(
      const ContainerID& containerId)
  {
    return Failure("Unsupported");
  }

  Future<ResourceStatistics> usage(
      const ContainerID& containerId)
  {
    return ResourceStatistics();
  }

  Future<ContainerStatus> status(
      const ContainerID& containerId)
  {
    return ContainerStatus();
  }

  Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId)
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

  Future<Option<mesos::slave::ContainerTermination>> destroy(
      const ContainerID& containerId)
  {
    if (!containers_.contains(containerId)) {
      return None();
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

    return termination;
  }

  // Additional destroy method for testing because we won't know the
  // ContainerID created for each container.
  Future<Option<mesos::slave::ContainerTermination>> destroy(
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
      return None();
    }

    return destroy(containerId.get());
  }

  Future<bool> kill(const ContainerID& containerId, int /* signal */)
  {
    return destroy(containerId)
      .then([]() { return true; });
  }

  Future<hashset<ContainerID>> containers()
  {
    return containers_.keys();
  }

  Future<Nothing> pruneImages(const vector<Image>& excludedImages)
  {
    return Nothing();
  }

private:
  struct ContainerData
  {
    Option<ExecutorID> executorId;
    Option<FrameworkID> frameworkId;

    process::Promise<mesos::slave::ContainerTermination> termination;
  };

  // We also store the terminated containers to allow callers to
  // "reap" the termination if a container is already destroyed.
  // This mimics the behavior of the mesos containerizer.
  hashmap<ContainerID, process::Owned<ContainerData>> containers_;
  hashmap<ContainerID, mesos::slave::ContainerTermination> terminatedContainers;

  struct ExecutorData
  {
    // Pre-HTTP executors.
    Executor* executor;
    process::Owned<MesosExecutorDriver> driver;

    // HTTP executors. Note that `mesos::v1::executor::Mesos`
    // requires that we provide it a shared pointer to the executor.
    shared_ptr<v1::MockHTTPExecutor> v1ExecutorMock;
    process::Owned<v1::executor::TestMesos> v1Library;
  };

  // TODO(bmahler): The test containerizer currently assumes that
  // executor IDs are unique across frameworks (see the constructors).
  hashmap<ExecutorID, process::Owned<ExecutorData>> executors;
};


TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    const shared_ptr<v1::MockHTTPExecutor>& executor)
  : process(new TestContainerizerProcess(executorId, executor))
{
  process::spawn(process.get());
  setup();
}


TestContainerizer::TestContainerizer(
    const hashmap<ExecutorID, Executor*>& executors)
  : process(new TestContainerizerProcess(executors))
{
  process::spawn(process.get());
  setup();
}


TestContainerizer::TestContainerizer(
    const ExecutorID& executorId,
    Executor* executor)
  : process(new TestContainerizerProcess({{executorId, executor}}))
{
  process::spawn(process.get());
  setup();
}


TestContainerizer::TestContainerizer(MockExecutor* executor)
  : process(new TestContainerizerProcess({{executor->id, executor}}))
{
  process::spawn(process.get());
  setup();
}


TestContainerizer::TestContainerizer()
  : process(new TestContainerizerProcess())
{
  process::spawn(process.get());
  setup();
}


TestContainerizer::~TestContainerizer()
{
  process::terminate(process.get());
  process::wait(process.get());
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
    .WillRepeatedly(Invoke(this, &TestContainerizer::_recover));

  EXPECT_CALL(*this, usage(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_usage));

  EXPECT_CALL(*this, status(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_status));

  EXPECT_CALL(*this, update(_, _, _))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_update));

  EXPECT_CALL(*this, launch(_, _, _, _))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_launch));

  EXPECT_CALL(*this, attach(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_attach));

  EXPECT_CALL(*this, wait(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_wait));

  EXPECT_CALL(*this, destroy(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_destroy));

  EXPECT_CALL(*this, kill(_, _))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_kill));

  EXPECT_CALL(*this, pruneImages(_))
    .WillRepeatedly(Invoke(this, &TestContainerizer::_pruneImages));
}


Future<Nothing> TestContainerizer::_recover(
    const Option<slave::state::SlaveState>& state)
{
  return process::dispatch(
    process.get(),
    &TestContainerizerProcess::recover,
    state);
}


Future<slave::Containerizer::LaunchResult> TestContainerizer::_launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const Option<string>& pidCheckpointPath)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::launch,
      containerId,
      containerConfig,
      environment,
      pidCheckpointPath);
}


Future<Connection> TestContainerizer::_attach(
    const ContainerID& containerId)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::attach,
      containerId);
}


Future<Nothing> TestContainerizer::_update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::update,
      containerId,
      resourceRequests,
      resourceLimits);
}


Future<ResourceStatistics> TestContainerizer::_usage(
    const ContainerID& containerId)
{
  return process::dispatch(
    process.get(),
    &TestContainerizerProcess::usage,
    containerId);
}


Future<ContainerStatus> TestContainerizer::_status(
    const ContainerID& containerId)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::status,
      containerId);
}


Future<Option<mesos::slave::ContainerTermination>> TestContainerizer::_wait(
    const ContainerID& containerId)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::wait,
      containerId);
}


Future<Option<mesos::slave::ContainerTermination>> TestContainerizer::_destroy(
    const ContainerID& containerId)
{
  // Need to disambiguate for the compiler.
  Future<Option<mesos::slave::ContainerTermination>> (
      TestContainerizerProcess::*destroy)(const ContainerID&) =
        &TestContainerizerProcess::destroy;

  return process::dispatch(
      process.get(),
      destroy,
      containerId);
}


Future<bool> TestContainerizer::_kill(
    const ContainerID& containerId,
    int signal)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::kill,
      containerId,
      signal);
}


Future<Option<mesos::slave::ContainerTermination>> TestContainerizer::destroy(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  // Need to disambiguate for the compiler.
  Future<Option<mesos::slave::ContainerTermination>> (
      TestContainerizerProcess::*destroy)(
          const FrameworkID&, const ExecutorID&) =
            &TestContainerizerProcess::destroy;

  return process::dispatch(
      process.get(),
      destroy,
      frameworkId,
      executorId);
}


Future<hashset<ContainerID>> TestContainerizer::containers()
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::containers);
}


Future<Nothing> TestContainerizer::_pruneImages(
    const vector<Image>& excludedImages)
{
  return process::dispatch(
      process.get(),
      &TestContainerizerProcess::pruneImages,
      excludedImages);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
