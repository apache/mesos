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

#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "master/allocator/mesos/allocator.hpp"

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::master::Master;

using mesos::master::detector::MasterDetector;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

using process::http::OK;

using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class MesosSchedulerDriverTest : public MesosTest {};


TEST_F(MesosSchedulerDriverTest, MetricsEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(registered);

  Future<process::http::Response> response =
    process::http::get(process::metrics::internal::metrics, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(parse);

  JSON::Object metrics = parse.get();

  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_messages"));
  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_dispatches"));

  driver.stop();
  driver.join();
}


// This action calls driver stop() followed by abort().
ACTION(StopAndAbort)
{
  arg0->stop();
  arg0->abort();
}


// This test verifies that when the scheduler calls stop() before
// abort(), no pending acknowledgements are sent.
TEST_F(MesosSchedulerDriverTest, DropAckIfStopCalledBeforeAbort)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // When an update is received, stop the driver and then abort it.
  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(StopAndAbort(),
                    FutureSatisfy(&statusUpdate)));

  // Ensure no status update acknowledgements are sent from the driver
  // to the master.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _ ,
      master.get()->pid);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.start();

  AWAIT_READY(statusUpdate);

  // Settle the clock to ensure driver finishes processing the status
  // update and sends acknowledgement if necessary. In this test it
  // shouldn't send an acknowledgement.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// Ensures that when a scheduler enables explicit acknowledgements
// on the driver, there are no implicit acknowledgements sent, and
// the call to 'acknowledgeStatusUpdate' sends the ack to the master.
TEST_F(MesosSchedulerDriverTest, ExplicitAcknowledgements)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure no status update acknowledgements are sent from the driver
  // to the master until the explicit acknowledgement is sent.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _ ,
      master.get()->pid);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.start();

  AWAIT_READY(status);

  // Settle the clock to ensure driver finishes processing the status
  // update, we want to ensure that no implicit acknowledgement gets
  // sent.
  Clock::pause();
  Clock::settle();

  // Now send the acknowledgement.
  Future<mesos::scheduler::Call> acknowledgement = FUTURE_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _,
      master.get()->pid);

  driver.acknowledgeStatusUpdate(status.get());

  AWAIT_READY(acknowledgement);

  driver.stop();
  driver.join();
}


// This test ensures that when explicit acknowledgements are enabled,
// acknowledgements for master-generated updates are dropped by the
// driver. We test this by creating an invalid task that uses no
// resources.
TEST_F(MesosSchedulerDriverTest, ExplicitAcknowledgementsMasterGeneratedUpdate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Ensure no status update acknowledgements are sent to the master.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _ ,
      master.get()->pid);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  // Launch a task using no resources.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  ASSERT_EQ(TASK_ERROR, status->state());
  ASSERT_EQ(TaskStatus::SOURCE_MASTER, status->source());
  ASSERT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());

  // Now send the acknowledgement.
  driver.acknowledgeStatusUpdate(status.get());

  // Settle the clock to ensure driver processes the acknowledgement,
  // which should get dropped due to having come from the master.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test ensures that the driver handles an empty slave id
// in an acknowledgement message by dropping it. The driver will
// log an error in this case (but we don't test for that). We
// generate a status with no slave id by performing reconciliation.
TEST_F(MesosSchedulerDriverTest, ExplicitAcknowledgementsUnsetSlaveID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Ensure no status update acknowledgements are sent to the master.
  EXPECT_NO_FUTURE_CALLS(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACKNOWLEDGE,
      _ ,
      master.get()->pid);

  driver.start();

  AWAIT_READY(registered);

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&update));

  // Peform reconciliation without using a slave id.
  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_task_id()->set_value("foo");
  status.set_state(TASK_RUNNING);

  statuses.push_back(status);

  driver.reconcileTasks(statuses);

  AWAIT_READY(update);
  ASSERT_EQ(TASK_LOST, update->state());
  ASSERT_EQ(TaskStatus::SOURCE_MASTER, update->source());
  ASSERT_EQ(TaskStatus::REASON_RECONCILIATION, update->reason());
  ASSERT_FALSE(update->has_slave_id());

  // Now send the acknowledgement.
  driver.acknowledgeStatusUpdate(update.get());

  // Settle the clock to ensure driver processes the acknowledgement,
  // which should get dropped due to the missing slave id.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
