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

#include <list>
#include <string>
#include <tuple>

#include <mesos/resources.hpp>
#include <mesos/version.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/stopwatch.hpp>

#include "common/protobuf_utils.hpp"

#include "tests/mesos.hpp"

using process::await;
using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::ProcessBase;
using process::Promise;
using process::spawn;
using process::terminate;
using process::UPID;

using std::cout;
using std::endl;
using std::list;
using std::make_tuple;
using std::string;
using std::tie;
using std::tuple;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

static SlaveInfo createSlaveInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources =
    Resources::parse("cpus:20;mem:10240").get();

  SlaveInfo slaveInfo;
  *(slaveInfo.mutable_resources()) = resources;
  *(slaveInfo.mutable_id()) = slaveId;
  *(slaveInfo.mutable_hostname()) = slaveId.value(); // Simulate the hostname.

  return slaveInfo;
}


static FrameworkInfo createFrameworkInfo(const FrameworkID& frameworkId)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  *(frameworkInfo.mutable_id()) = frameworkId;

  return frameworkInfo;
}


static TaskInfo createTaskInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources = Resources::parse("cpus:0.1;mem:5").get();

  TaskInfo taskInfo = createTask(
      slaveId,
      resources,
      "dummy command");

  Labels* labels = taskInfo.mutable_labels();

  for (size_t i = 0; i < 10; i++) {
    const string index = stringify(i);
    *(labels->add_labels()) =
      protobuf::createLabel("key" + index, "value" + index);
  }

  return taskInfo;
}


// A fake agent currently just for testing reregisterations.
class TestSlaveProcess : public ProtobufProcess<TestSlaveProcess>
{
public:
  TestSlaveProcess(
      const UPID& _masterPid,
      const SlaveID& _slaveId,
      size_t _frameworksPerAgent,
      size_t _tasksPerFramework,
      size_t _completedFrameworksPerAgent,
      size_t _tasksPerCompletedFramework)
    : ProcessBase(process::ID::generate("test-slave")),
      masterPid(_masterPid),
      slaveId(_slaveId),
      frameworksPerAgent(_frameworksPerAgent),
      tasksPerFramework(_tasksPerFramework),
      completedFrameworksPerAgent(_completedFrameworksPerAgent),
      tasksPerCompletedFramework(_tasksPerCompletedFramework) {}

  void initialize() override
  {
    install<SlaveReregisteredMessage>(&Self::reregistered);
    install<PingSlaveMessage>(
        &Self::ping,
        &PingSlaveMessage::connected);

    // Prepare `ReregisterSlaveMessage` which simulates the real world scenario:
    // TODO(xujyan): Notable things missing include:
    // - `ExecutorInfo`s
    // - Task statuses
    SlaveInfo slaveInfo = createSlaveInfo(slaveId);
    message.mutable_slave()->Swap(&slaveInfo);
    message.set_version(MESOS_VERSION);

    // Used for generating framework IDs.
    size_t id = 0;
    for (; id < frameworksPerAgent; id++) {
      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      message.add_frameworks()->Swap(&framework);

      for (size_t j = 0; j < tasksPerFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_RUNNING,
            frameworkId);
        message.add_tasks()->Swap(&task);
      }
    }

    for (; id < frameworksPerAgent + completedFrameworksPerAgent; id++) {
      Archive::Framework* completedFramework =
        message.add_completed_frameworks();

      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      completedFramework->mutable_framework_info()->Swap(&framework);

      for (size_t j = 0; j < tasksPerCompletedFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_FINISHED,
            frameworkId);
        completedFramework->add_tasks()->Swap(&task);
      }
    }
  }

  Future<Nothing> reregister()
  {
    send(masterPid, message);
    return promise.future();
  }

  TestSlaveProcess(const TestSlaveProcess& other) = delete;
  TestSlaveProcess& operator=(const TestSlaveProcess& other) = delete;

private:
  void reregistered(const SlaveReregisteredMessage&)
  {
    promise.set(Nothing());
  }

  // We need to answer pings to keep the agent registered.
  void ping(const UPID& from, bool)
  {
    send(from, PongSlaveMessage());
  }

  const UPID masterPid;
  const SlaveID slaveId;
  const size_t frameworksPerAgent;
  const size_t tasksPerFramework;
  const size_t completedFrameworksPerAgent;
  const size_t tasksPerCompletedFramework;

  ReregisterSlaveMessage message;
  Promise<Nothing> promise;
};


class TestSlave
{
public:
  TestSlave(
      const UPID& masterPid,
      const SlaveID& slaveId,
      size_t frameworksPerAgent,
      size_t tasksPerFramework,
      size_t completedFrameworksPerAgent,
      size_t tasksPerCompletedFramework)
    : process(new TestSlaveProcess(
          masterPid,
          slaveId,
          frameworksPerAgent,
          tasksPerFramework,
          completedFrameworksPerAgent,
          tasksPerCompletedFramework))
  {
    spawn(process.get());
  }

  ~TestSlave()
  {
    terminate(process.get());
    process::wait(process.get());
  }

  Future<Nothing> reregister()
  {
    return dispatch(process.get(), &TestSlaveProcess::reregister);
  }

private:
  Owned<TestSlaveProcess> process;
};


class MasterFailover_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<size_t, size_t, size_t, size_t, size_t>> {};


// The value tuples are defined as:
// - agentCount
// - frameworksPerAgent
// - tasksPerFramework (per agent)
// - completedFrameworksPerAgent
// - tasksPerCompletedFramework (per agent)
INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCount,
    MasterFailover_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(2000, 5, 10, 5, 10),
        make_tuple(2000, 5, 20, 0, 0),
        make_tuple(20000, 1, 5, 0, 0)));


// This test measures the time from all agents start to reregister to
// to when all have received `SlaveReregisteredMessage`.
TEST_P(MasterFailover_BENCHMARK_Test, AgentReregistrationDelay)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
      frameworksPerAgent,
      tasksPerFramework,
      completedFrameworksPerAgent,
      tasksPerCompletedFramework) = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  // Use replicated log so it better simulates the production scenario.
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  list<TestSlave> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.emplace_back(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework);
  }

  // Make sure all agents are ready to reregister before we start the stopwatch.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  list<Future<Nothing>> reregistered;

  cout << "Starting reregistration for all agents" << endl;

  // Measure the time for all agents to receive `SlaveReregisteredMessage`.
  Stopwatch watch;
  watch.start();

  foreach (TestSlave& slave, slaves) {
    reregistered.push_back(slave.reregister());
  }

  await(reregistered).await();

  watch.stop();

  cout << "Reregistered " << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks in "
       << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
