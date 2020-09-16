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
#include "master/detector/standalone.hpp"
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

using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Return;
using ::testing::InSequence;

namespace mesos {
namespace internal {
namespace tests {

class MesosSchedulerDriverTest : public MesosTest {};


// Ensures that the scheduler driver can backoff correctly when
// framework failover time is set to zero.
TEST_F(MesosSchedulerDriverTest, RegistrationWithZeroFailoverTime)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(0);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Clock::pause();

  {
    InSequence inSequence;

    // Drop the first SUBSCRIBE call so that the driver retries.
    DROP_CALL(
        mesos::scheduler::Call(),
        mesos::scheduler::Call::SUBSCRIBE,
        _,
        master.get()->pid);

    // Settling the clock ensures that if a retried SUBSCRIBE
    // call is enqueued it will be processed. Since the backoff
    // is non-zero no new SUBSCRIBE call should be sent when
    // the clock is paused.
    EXPECT_NO_FUTURE_CALLS(
        mesos::scheduler::Call(),
        mesos::scheduler::Call::SUBSCRIBE,
        _,
        master.get()->pid);
  }

  driver.start();

  Clock::settle();

  driver.stop();
  driver.join();
}


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
      _,
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
      _,
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
      _,
      master.get()->pid);

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

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


// This test ensures that re-registration preserves suppression of offers.
TEST_F(MesosSchedulerDriverTest, ReregisterAfterSuppress)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  ::mesos::master::detector::StandaloneMasterDetector detector;
  detector.appoint(master.get()->pid);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, DEFAULT_FRAMEWORK_INFO);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Sanity check: the re-registration should occur once.
  EXPECT_CALL(sched, reregistered(&driver, _))
    .Times(1);

  // We should never get offers, despite of the re-registration.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(AtMost(0));

  driver.start();

  // Driver might take a different path if suppressOffers() is called in a
  // disconnected state. We choose a typical path by waiting for registration.
  AWAIT_READY(registered);

  driver.suppressOffers();
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Add a slave to get offers if they are not suppressed.
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  // Trigger re-registration.
  detector.appoint(None());
  detector.appoint(master.get()->pid);

  // Trigger allocation to ensure that offers are still suppressed.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
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
      _,
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


// This test ensures that the driver can register with a suppressed role.
TEST_F(MesosSchedulerDriverTest, RegisterWithSuppressedRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      {"role2"},
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  Future<vector<Offer>> offers;

  // We should get offers EXACTLY once.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());
  ASSERT_EQ("role1", offers.get()[0].allocation_info().role());

  Filters filter1day;
  filter1day.set_refuse_seconds(Days(1).secs());

  driver.declineOffer(offers.get()[0].id(), filter1day);
  Clock::pause();
  Clock::settle();

  // Trigger allocation to ensure that role2 is suppressed. We should get no
  // more offers.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test ensures that suppressOffers() can suppress
// one role of a multi-role framework.
//
// We subscribe a framework with two roles, suppress the first one, decline
// the offer for the second one, and expect to receive no offers after that.
TEST_F(MesosSchedulerDriverTest, SuppressSingleRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers;

  // We should get offers EXACTLY once - for role2, which we don't suppress.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(registered);

  driver.suppressOffers({"role1"});

  // Ensure the suppression is processed by the master before
  // adding the agent, otherwise the offer could go to role1.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Decline an offer for role2 and set a long filter.
  Filters filter1day;
  filter1day.set_refuse_seconds(Days(1).secs());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());
  ASSERT_EQ("role2", offers.get()[0].allocation_info().role());

  driver.declineOffer(offers.get()[0].id(), filter1day);

  // Trigger allocation and expect no offers
  // (we are checking that role1 is indeed suppressed).
  Clock::pause();
  Clock::settle();

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test ensures that reviveOffers() can unsuppress
// one role of a multi-role framework.
//
// We subscribe a framework with two roles, decline offers
// for both, call reviveOffers() for the second role and
// check that only the first one is filtered after that.
TEST_F(MesosSchedulerDriverTest, ReviveSingleRole)
{
  mesos::internal::master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2));

  driver.start();

  // Decline offers for both roles and set a long filter.
  Filters filter1day;
  filter1day.set_refuse_seconds(Days(1).secs());

  set<string> declinedOfferRoles;

  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1->size());
  driver.declineOffer(offers1.get()[0].id(), filter1day);
  declinedOfferRoles.emplace(offers1.get()[0].allocation_info().role());

  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2->size());
  driver.declineOffer(offers2.get()[0].id(), filter1day);
  declinedOfferRoles.emplace(offers2.get()[0].allocation_info().role());

  // Sanity check: we should have responed to offers for both roles.
  ASSERT_EQ(set<string>({"role1", "role2"}), declinedOfferRoles);

  // In addition to setting a filter, suppress role2 (to check that
  // reviveOffers() not only removes filters, but also unsuppresses roles).
  *frameworkInfo.mutable_id() = frameworkId.get();
  driver.updateFramework(frameworkInfo, {"role2"}, {});

  // Wait for updateFramework() to be dispatched to the allocator.
  // Otherwise, REVIVE might be processed by the allocator before the update.
  Clock::pause();
  Clock::settle();

  // After reviving role2 we expect offers EXACTLY once, for role2.
  Future<vector<Offer>> offersOnRevival;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offersOnRevival));

  driver.reviveOffers({"role2"});

  AWAIT_READY(offersOnRevival);
  ASSERT_EQ(1u, offersOnRevival->size());
  ASSERT_EQ("role2", offersOnRevival.get()[0].allocation_info().role());

  // Decline the offer for role2 and set a filter.
  driver.declineOffer(offersOnRevival.get()[0].id(), filter1day);
  Clock::settle();

  // Trigger allocation to ensure that role1 still has a filter.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  driver.stop();
  driver.join();
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
