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
#include <type_traits>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/mesos/isolators/network/ports.hpp"

#include "tests/mesos.hpp"

using mesos::internal::slave::NetworkPortsIsolatorProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;

using std::string;
using std::vector;

using namespace routing::diagnosis;

namespace mesos {
namespace internal {
namespace tests {

class NetworkPortsIsolatorTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    std::srand(std::time(0));
  }

  // Wait until a status update is received and subsequently acknowledged.
  // If we don't wait for the acknowledgement, then advancing the clock can
  // cause the agent to time out on receiving the acknowledgement, at which
  // point it will re-send and the test will intercept an unexpected duplicate
  // status update.
  void awaitStatusUpdateAcked(Future<TaskStatus>& status)
  {
    Future<Nothing> ack =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

    AWAIT_READY(status);
    AWAIT_READY(ack);
  }

  // Expect that the TaskStatus is reporting a successful health check.
  void expectHealthyStatus(const TaskStatus& status)
  {
    EXPECT_EQ(TASK_RUNNING, status.state());

    EXPECT_EQ(
        TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
        status.reason());

    ASSERT_TRUE(status.has_healthy());
    EXPECT_TRUE(status.healthy());
  }

  // Expect that the TaskStatus is a container limitation that tells us
  // about a single ports resource.
  void expectPortsLimitation(
      const TaskStatus& status,
      const Option<uint64_t>& port = None())
  {
    EXPECT_EQ(TaskStatus::REASON_CONTAINER_LIMITATION, status.reason());

    ASSERT_TRUE(status.has_limitation()) << JSON::protobuf(status);

    Resources limit = Resources(status.limitation().resources());

    EXPECT_EQ(1u, limit.size());
    ASSERT_SOME(limit.ports());

    if (port.isSome()) {
      ASSERT_EQ(1, limit.ports()->range().size());
      EXPECT_EQ(port.get(), limit.ports()->range(0).begin());
      EXPECT_EQ(port.get(), limit.ports()->range(0).end());
    }
  }
};


// Select a random port from an offer.
static uint16_t selectRandomPort(const Resources& resources)
{
  Value::Range ports = resources.ports()->range(0);
  return ports.begin() + std::rand() % (ports.end() - ports.begin() + 1);
}


// Select a random port that is not the same as the one given.
static uint16_t selectOtherPort(const Resources& resources, uint16_t port)
{
  uint16_t selected;

  do {
    selected = selectRandomPort(resources);
  } while (selected == port);

  return selected;
}


template <typename T>
static void addTcpHealthCheck(T& taskInfo, uint16_t port)
{
  auto* checkInfo = taskInfo.mutable_health_check();

  checkInfo->set_type(std::remove_pointer<decltype(checkInfo)>::type::TCP);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_grace_period_seconds(10);
  checkInfo->set_interval_seconds(1);
  checkInfo->mutable_tcp()->set_port(port);
}


// This test verifies that we can correctly detect sockets that
// a process is listening on. We take advantage of the fact that
// libprocess always implicitly listens on a socket, so we can
// query our current PID for listening sockets and verify that
// result against the libprocess address.
TEST(NetworkPortsIsolatorUtilityTest, QueryProcessSockets)
{
  Try<hashmap<uint32_t, socket::Info>> listeners =
    NetworkPortsIsolatorProcess::getListeningSockets();

  ASSERT_SOME(listeners);
  EXPECT_GT(listeners->size(), 0u);

  foreachvalue (const socket::Info& info, listeners.get()) {
    EXPECT_SOME(info.sourceIP);
    EXPECT_SOME(info.sourcePort);
  }

  Try<std::vector<uint32_t>> socketInodes =
    NetworkPortsIsolatorProcess::getProcessSockets(getpid());

  ASSERT_SOME(socketInodes);
  EXPECT_GT(socketInodes->size(), 0u);

  vector<socket::Info> socketInfos;

  // Collect the Info for our own listening sockets.
  foreach (uint32_t inode, socketInodes.get()) {
    if (listeners->contains(inode)) {
        socketInfos.push_back(listeners->at(inode));
    }
  }

  // libprocess always listens on a socket, so the fact that we
  // are running implies that we should at least find out about
  // the libprocess socket.
  EXPECT_GT(socketInfos.size(), 0u);

  bool matched = false;
  process::network::inet::Address processAddress = process::address();

  foreach (const auto& info, socketInfos) {
    // We can only match on the port, since libprocess will typically
    // indicate that it is listening on the ANY address (i.e. 0.0.0.0)
    // but the socket diagnostics will publish the actual address of a
    // network interface.
    if (ntohs(info.sourcePort.get()) == processAddress.port) {
      matched = true;
    }
  }

  // Verify that we matched the libprocess address in the set of
  // listening sockets for this process.
  EXPECT_TRUE(matched) << "Unmatched libprocess address "
                       << processAddress;
}


// This test verifies that the `network/ports` isolator throws
// an error unless the `linux` launcher is being used.
TEST_F(NetworkPortsIsolatorTest, ROOT_IsolatorFlags)
{
  StandaloneMasterDetector detector;

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";

  Try<Owned<cluster::Slave>> slave = Owned<cluster::Slave>();

  flags.launcher = "posix";
  slave = StartSlave(&detector, flags);
  ASSERT_ERROR(slave);

  flags.launcher = "linux";
  slave = StartSlave(&detector, flags);
  ASSERT_SOME(slave);
}


// libprocess always listens on a port when it is initialized
// with no control over whether task resources are allocated
// for that port. This test verifies that a task that uses the
// command executor will always be killed due to the libprocess
// port even when it doesn't open any ports itself.
TEST_F(NetworkPortsIsolatorTest, ROOT_CommandExecutorPorts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";
  flags.launcher = "linux";
  flags.check_agent_port_range_only = false;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      "sleep 10000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.launchTasks(offer.id(), {task});

  awaitStatusUpdateAcked(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  awaitStatusUpdateAcked(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  Future<Nothing> check =
    FUTURE_DISPATCH(_, &NetworkPortsIsolatorProcess::check);

  Clock::pause();
  Clock::advance(flags.container_ports_watch_interval);

  AWAIT_READY(check);

  Clock::settle();
  Clock::resume();

  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&failedStatus));

  // Even though the task itself never listened on any ports, we expect
  // that it gets killed because the isolator detects the libprocess
  // port the command executor is listening on.
  AWAIT_READY(failedStatus);
  EXPECT_EQ(task.task_id(), failedStatus->task_id());
  EXPECT_EQ(TASK_FAILED, failedStatus->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, failedStatus->source());
  expectPortsLimitation(failedStatus.get());

  driver.stop();
  driver.join();
}


// This test verifies that a task that correctly listens on
// ports for which it holds resources is allowed to run and it
// not killed by the `network/ports` isolator.
TEST_F(NetworkPortsIsolatorTest, ROOT_NC_AllocatedPorts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";
  flags.launcher = "linux";

  // Watch only the agent ports resources range because we want this
  // test to trigger on the nc command, not on the command executor.
  flags.check_agent_port_range_only = true;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  Resources resources(offers.get()[0].resources());

  // Make sure we have a `ports` resource.
  ASSERT_SOME(resources.ports());
  ASSERT_LE(1, resources.ports()->range().size());

  uint16_t taskPort = selectRandomPort(resources);

  resources = Resources::parse(
      "cpus:1;mem:32;"
      "ports:[" + stringify(taskPort) + "," + stringify(taskPort) + "]").get();

  // Use "nc -k" so nc keeps running after accepting the healthcheck connection.
  TaskInfo task = createTask(
      offer.slave_id(),
      resources,
      "nc -k -l " + stringify(taskPort));

  addTcpHealthCheck(task, taskPort);

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> healthStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&healthStatus));

  driver.launchTasks(offer.id(), {task});

  awaitStatusUpdateAcked(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  awaitStatusUpdateAcked(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  awaitStatusUpdateAcked(healthStatus);
  ASSERT_EQ(task.task_id(), healthStatus->task_id());
  expectHealthyStatus(healthStatus.get());

  Future<Nothing> check =
    FUTURE_DISPATCH(_, &NetworkPortsIsolatorProcess::check);

  Clock::pause();
  Clock::advance(flags.container_ports_watch_interval);

  AWAIT_READY(check);

  Clock::settle();
  Clock::resume();

  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killedStatus));

  driver.killTask(task.task_id());

  AWAIT_READY(killedStatus);
  EXPECT_EQ(task.task_id(), killedStatus->task_id());
  EXPECT_EQ(TASK_KILLED, killedStatus->state());

  driver.stop();
  driver.join();
}


// This test verifies that if the agent has an empty ports
// resource, and the check_agent_port_range_only flag is enabled,
// a task using an arbitrary port is allowed to start up and
// become healthy. This is correct because it effectively reduces
// the set of ports we are protecting to zero.
TEST_F(NetworkPortsIsolatorTest, ROOT_NC_NoPortsResource)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";
  flags.launcher = "linux";

  // Omit the default ports from the agent resources.
  flags.resources = R"(
  [
    {
      "name": "cpus",
      "type": "SCALAR",
      "scalar": {
        "value": 2
      }
    },
    {
      "name": "gpus",
      "type": "SCALAR",
      "scalar": {
        "value": 0
      }
    },
    {
      "name": "mem",
      "type": "SCALAR",
      "scalar": {
        "value": 1024
      }
    },
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": {
        "value": 1024
      }
    },
    {
      "name": "ports",
      "type": "RANGES",
      "ranges": {
      }
    }
  ]
  )";

  // Watch only the agent ports resources range because we want this
  // test to trigger on the nc command, not on the command executor.
  flags.check_agent_port_range_only = true;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  Resources resources(offers.get()[0].resources());

  // Make sure we do not have a `ports` resource in the offer.
  ASSERT_NONE(resources.ports());

  // Select a random task port from the default range.
  resources = Resources::parse(
      "ports",
      stringify(slave::DEFAULT_PORTS),
      flags.default_role).get();

  uint16_t taskPort = selectRandomPort(resources);

  resources = Resources::parse("cpus:1;mem:32").get();

  // Use "nc -k" so nc keeps running after accepting the healthcheck connection.
  TaskInfo task = createTask(
      offer.slave_id(),
      resources,
      "nc -k -l " + stringify(taskPort));

  addTcpHealthCheck(task, taskPort);

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> healthStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&healthStatus));

  driver.launchTasks(offer.id(), {task});

  awaitStatusUpdateAcked(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  awaitStatusUpdateAcked(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  awaitStatusUpdateAcked(healthStatus);
  ASSERT_EQ(task.task_id(), healthStatus->task_id());
  expectHealthyStatus(healthStatus.get());

  Future<Nothing> check =
    FUTURE_DISPATCH(_, &NetworkPortsIsolatorProcess::check);

  Clock::pause();
  Clock::advance(flags.container_ports_watch_interval);

  AWAIT_READY(check);

  Clock::settle();
  Clock::resume();

  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killedStatus));

  driver.killTask(task.task_id());

  AWAIT_READY(killedStatus);
  EXPECT_EQ(task.task_id(), killedStatus->task_id());
  EXPECT_EQ(TASK_KILLED, killedStatus->state());

  driver.stop();
  driver.join();
}


// This test verifies that the isolator correctly defaults the agent ports
// resource when the operator doesn't specify any ports. We verify that a
// task that uses an unallocated port in the agent's offer is detected and
// killed by a container limitation.
TEST_F(NetworkPortsIsolatorTest, ROOT_NC_DefaultPortsResource)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";
  flags.launcher = "linux";

  // Clear resources set by CreateSlaveFlags() to force the agent and
  // isolator to apply built-in defaults.
  flags.resources = None();

  // Watch only the agent ports resources range because we want this
  // test to trigger on the nc command, not on the command executor.
  flags.check_agent_port_range_only = true;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  // Make sure we have a `ports` resource.
  Resources resources(offer.resources());
  ASSERT_SOME(resources.ports());
  ASSERT_LE(1, resources.ports()->range().size());

  uint16_t taskPort = selectRandomPort(resources);
  uint16_t usedPort = selectOtherPort(resources, taskPort);

  resources = Resources::parse(
      "cpus:1;mem:32;"
      "ports:[" + stringify(taskPort) + "," + stringify(taskPort) + "]").get();

  // Use "nc -k" so nc keeps running after accepting the healthcheck connection.
  TaskInfo task = createTask(
      offer.slave_id(),
      resources,
      "nc -k -l " + stringify(usedPort));

  addTcpHealthCheck(task, usedPort);

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> healthStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&healthStatus));

  driver.launchTasks(offer.id(), {task});

  awaitStatusUpdateAcked(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  awaitStatusUpdateAcked(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  awaitStatusUpdateAcked(healthStatus);
  ASSERT_EQ(task.task_id(), healthStatus->task_id());
  expectHealthyStatus(healthStatus.get());

  Future<Nothing> check =
    FUTURE_DISPATCH(_, &NetworkPortsIsolatorProcess::check);

  Clock::pause();
  Clock::advance(flags.container_ports_watch_interval);

  AWAIT_READY(check);

  Clock::settle();
  Clock::resume();

  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&failedStatus));

  // We expect that the task will get killed by the isolator.
  AWAIT_READY(failedStatus);
  EXPECT_EQ(task.task_id(), failedStatus->task_id());
  EXPECT_EQ(TASK_FAILED, failedStatus->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, failedStatus->source());
  expectPortsLimitation(failedStatus.get(), usedPort);

  driver.stop();
  driver.join();
}


// This test verifies that a task that listens on a port for which it has
// no resources is detected and killed by a container limitation.
TEST_F(NetworkPortsIsolatorTest, ROOT_NC_UnallocatedPorts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/ports";
  flags.launcher = "linux";

  // Watch only the agent ports resources range because we want this
  // test to trigger on the nc command, not on the command executor.
  flags.check_agent_port_range_only = true;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  // Make sure we have a `ports` resource.
  Resources resources(offer.resources());
  ASSERT_SOME(resources.ports());
  ASSERT_LE(1, resources.ports()->range().size());

  uint16_t taskPort = selectRandomPort(resources);
  uint16_t usedPort = selectOtherPort(resources, taskPort);

  resources = Resources::parse(
      "cpus:1;mem:32;"
      "ports:[" + stringify(taskPort) + "," + stringify(taskPort) + "]").get();

  // Launch a task that uses a port that it hasn't been allocated.  Use
  // "nc -k" so nc keeps running after accepting the healthcheck connection.
  TaskInfo task = createTask(
      offer.slave_id(),
      resources,
      "nc -k -l " + stringify(usedPort));

  addTcpHealthCheck(task, usedPort);

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> healthStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&healthStatus));

  driver.launchTasks(offer.id(), {task});

  awaitStatusUpdateAcked(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  awaitStatusUpdateAcked(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  awaitStatusUpdateAcked(healthStatus);
  ASSERT_EQ(task.task_id(), healthStatus->task_id());
  expectHealthyStatus(healthStatus.get());

  Future<Nothing> check =
    FUTURE_DISPATCH(_, &NetworkPortsIsolatorProcess::check);

  Clock::pause();
  Clock::advance(flags.container_ports_watch_interval);

  AWAIT_READY(check);

  Clock::settle();
  Clock::resume();

  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&failedStatus));

  // We expect that the task will get killed by the isolator.
  AWAIT_READY(failedStatus);
  EXPECT_EQ(task.task_id(), failedStatus->task_id());
  EXPECT_EQ(TASK_FAILED, failedStatus->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, failedStatus->source());
  expectPortsLimitation(failedStatus.get(), usedPort);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
