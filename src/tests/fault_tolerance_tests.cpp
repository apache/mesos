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
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authentication/authentication.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/json.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/contender/standalone.hpp"
#include "master/contender/zookeeper.hpp"

#include "master/detector/standalone.hpp"
#include "master/detector/zookeeper.hpp"

#include "sched/constants.hpp"

#include "slave/constants.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos::internal::protobuf;

using mesos::internal::master::Master;

using mesos::internal::scheduler::DEFAULT_REGISTRATION_BACKOFF_FACTOR;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;
using process::UPID;

using process::http::OK;
using process::http::Response;

using std::string;
using std::vector;

using testing::_;
using testing::AnyOf;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class FaultToleranceTest : public MesosTest {};


// This test ensures that a framework connecting with a
// failed over master gets a registered callback.
// Note that this behavior might change in the future and
// the scheduler might receive a reregistered callback.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FaultToleranceTest, MasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<Nothing> registered1;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered1));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  AWAIT_READY(registered1);

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate failed over master by restarting the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, _);

  Future<Nothing> registered2;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  // Simulate a new master detected message to the scheduler.
  detector.appoint(master.get()->pid);

  // Scheduler should retry authentication.
  AWAIT_READY(authenticateMessage);

  // Framework should get a registered callback.
  AWAIT_READY(registered2);

  driver.stop();
  driver.join();
}


// This test ensures that a failed over master recovers completed tasks
// from a slave's re-registration when the slave thinks the framework has
// completed (but the framework has not actually completed yet from master's
// point of view).
TEST_F(FaultToleranceTest, ReregisterCompletedFrameworks)
{
  // Step 1. Start Master and Slave.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor executor(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&executor);

  StandaloneMasterDetector slaveDetector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector, &containerizer);
  ASSERT_SOME(slave);

  // Verify master/slave have 0 completed/running frameworks.
  Future<Response> masterState = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterState);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterState);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(masterState->body);
  ASSERT_SOME(parse);
  JSON::Object masterJSON = parse.get();

  EXPECT_EQ(
      0u,
      masterJSON.values["completed_frameworks"]
        .as<JSON::Array>().values.size());
  EXPECT_EQ(
      0u,
      masterJSON.values["frameworks"].as<JSON::Array>().values.size());

  // Step 2. Create/start framework.
  StandaloneMasterDetector schedDetector(master.get()->pid);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);
  EXPECT_NE("", frameworkId->value());
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Step 3. Create/launch a task.
  TaskInfo task =
    createTask(offers.get()[0], "sleep 10000", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(executor, registered(_, _, _, _));
  EXPECT_CALL(executor, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Verify master and slave recognize the running task/framework.
  masterState = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterState);

  parse = JSON::parse<JSON::Object>(masterState->body);
  ASSERT_SOME(parse);
  masterJSON = parse.get();

  EXPECT_EQ(
      0u,
      masterJSON.values["completed_frameworks"]
        .as<JSON::Array>().values.size());
  EXPECT_EQ(
      1u,
      masterJSON.values["frameworks"].as<JSON::Array>().values.size());

  Future<Response> slaveState = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, slaveState);

  parse = JSON::parse<JSON::Object>(slaveState->body);
  ASSERT_SOME(parse);
  JSON::Object slaveJSON = parse.get();

  EXPECT_EQ(
      0u,
      slaveJSON.values["completed_frameworks"].as<JSON::Array>().values.size());
  EXPECT_EQ(
      1u,
      slaveJSON.values["frameworks"].as<JSON::Array>().values.size());

  // Step 4. Kill task.
  EXPECT_CALL(executor, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  ASSERT_EQ(TASK_KILLED, statusKilled->state());

  // At this point, the task is killed, but the framework is still
  // running.  This is because the executor has to time-out before
  // it exits.
  masterState = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterState);

  parse = JSON::parse<JSON::Object>(masterState->body);
  ASSERT_SOME(parse);
  masterJSON = parse.get();

  EXPECT_EQ(
      0u,
      masterJSON.values["completed_frameworks"]
        .as<JSON::Array>().values.size());
  EXPECT_EQ(
      1u,
      masterJSON.values["frameworks"].as<JSON::Array>().values.size());

  slaveState = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, slaveState);

  parse = JSON::parse<JSON::Object>(slaveState->body);
  ASSERT_SOME(parse);
  slaveJSON = parse.get();

  EXPECT_EQ(
      0u,
      slaveJSON.values["completed_frameworks"].as<JSON::Array>().values.size());
  EXPECT_EQ(
      1u,
      slaveJSON.values["frameworks"].as<JSON::Array>().values.size());

  // Step 5. Kill the executor.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Induce an ExitedExecutorMessage from the slave.
  containerizer.destroy(
      frameworkId.get(), DEFAULT_EXECUTOR_INFO.executor_id());

  AWAIT_READY(executorTerminated);

  AWAIT_READY(executorLost);

  // Verify slave sees completed framework.
  slaveState = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, slaveState);

  parse = JSON::parse<JSON::Object>(slaveState->body);
  ASSERT_SOME(parse);
  slaveJSON = parse.get();

  EXPECT_EQ(
      1u,
      slaveJSON.values["completed_frameworks"].as<JSON::Array>().values.size());
  EXPECT_EQ(
      0u,
      slaveJSON.values["frameworks"].as<JSON::Array>().values.size());

  EXPECT_CALL(sched, disconnected(&driver));

  // Step 6. Simulate failed over master by restarting the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Step 7. Simulate a framework re-registration with a failed over master.
  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  schedDetector.appoint(master.get()->pid);

  AWAIT_READY(registered);

  // Step 8. Simulate a slave re-registration with a failed over master.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a new master detected message to the slave.
  slaveDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Verify that the master doesn't add the completed framework
  // reported by the slave to "frameworks.completed".
  Clock::pause();
  Clock::settle();
  Clock::resume();

  masterState = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterState);

  parse = JSON::parse<JSON::Object>(masterState->body);
  ASSERT_SOME(parse);
  masterJSON = parse.get();

  EXPECT_EQ(
      0u,
      masterJSON.values["completed_frameworks"]
        .as<JSON::Array>().values.size());
  EXPECT_EQ(
      1u,
      masterJSON.values["frameworks"].as<JSON::Array>().values.size());

  driver.stop();
  driver.join();
}


TEST_F(FaultToleranceTest, SchedulerFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  // Scheduler1's expectations.
  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());
}


// This test verifies that a framework attempting to reregister
// after its failover timeout has elapsed is disallowed.
TEST_F(FaultToleranceTest, SchedulerReregisterAfterFailoverTimeout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(frameworkId);

  Future<Nothing> deactivateFramework = FUTURE_DISPATCH(
      _, &master::allocator::MesosAllocatorProcess::deactivateFramework);

  Future<Nothing> frameworkFailoverTimeout =
    FUTURE_DISPATCH(_, &Master::frameworkFailoverTimeout);

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, _))
    .WillOnce(FutureSatisfy(&sched1Error));

  // Simulate framework disconnection.
  ASSERT_TRUE(process::inject::exited(
      frameworkRegisteredMessage->to, master.get()->pid));

  // Wait until master schedules the framework for removal.
  AWAIT_READY(deactivateFramework);

  // Simulate framework failover timeout.
  Clock::pause();
  Clock::settle();

  Try<Duration> failoverTimeout =
    Duration::create(FrameworkInfo().failover_timeout());

  ASSERT_SOME(failoverTimeout);
  Clock::advance(failoverTimeout.get());

  // Wait until master actually marks the framework as completed.
  AWAIT_READY(frameworkFailoverTimeout);

  AWAIT_READY(sched1Error);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .Times(0);

  Future<Nothing> sched2Error;
  EXPECT_CALL(sched2, error(&driver2, _))
    .WillOnce(FutureSatisfy(&sched2Error));

  driver2.start();

  // Framework should get 'Error' message because the framework
  // with this id is marked as completed.
  AWAIT_READY(sched2Error);

  driver2.stop();
  driver2.join();
  driver1.stop();
  driver1.join();
}


// This test verifies that a framework attempting to reregister
// after it is unregistered is disallowed.
TEST_F(FaultToleranceTest, SchedulerReregisterAfterUnregistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(frameworkId);

  Future<Nothing> removeFramework = FUTURE_DISPATCH(
      _, &master::allocator::MesosAllocatorProcess::removeFramework);

  // Unregister the framework.
  driver1.stop();
  driver1.join();

  // Wait until master actually marks the framework as completed.
  AWAIT_READY(removeFramework);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .Times(0);

  Future<Nothing> sched2Error;
  EXPECT_CALL(sched2, error(&driver2, _))
    .WillOnce(FutureSatisfy(&sched2Error));

  driver2.start();

  // Framework should get 'Error' message because the framework
  // with this id is marked as completed.
  AWAIT_READY(sched2Error);

  driver2.stop();
  driver2.join();
}


// This test was added to cover a fix for MESOS-659.
// Here, we drop the initial FrameworkReregisteredMessage from the
// master, so that the scheduler driver retries the initial failover
// re-registration. Previously, this caused a "Framework failed over"
// to be sent to the new scheduler driver!
TEST_F(FaultToleranceTest, SchedulerFailoverRetriedReregistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Clock::pause();

  // Drop the initial FrameworkRegisteredMessage to the failed over
  // scheduler. This ensures the scheduler driver will retry the
  // registration.
  Future<process::Message> reregistrationMessage = DROP_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get()->pid, _);

  // There should be no error received, the master sends the error
  // prior to sending the FrameworkRegisteredMessage so we don't
  // need to wait to ensure this does not occur.
  EXPECT_CALL(sched2, error(&driver2, "Framework failed over"))
    .Times(0);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(reregistrationMessage);

  // Trigger the re-registration retry instantly to avoid blocking the test.
  Clock::advance(DEFAULT_REGISTRATION_BACKOFF_FACTOR);

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());
  Clock::resume();
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(
    FaultToleranceTest,
    FrameworkReliableRegistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, master.get()->pid);

  // Drop the first framework registered message, allow subsequent messages.
  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    DROP_PROTOBUF(FrameworkRegisteredMessage(), master.get()->pid, _);

  driver.start();

  // Ensure authentication occurs.
  AWAIT_READY(authenticateMessage);

  AWAIT_READY(frameworkRegisteredMessage);

  // Trigger the re-registration retry instantly to avoid blocking the test.
  Clock::pause();
  Clock::advance(DEFAULT_REGISTRATION_BACKOFF_FACTOR);

  AWAIT_READY(registered); // Ensures registered message is received.

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test ensures framework reregisters with master after failover.
// Previous offers are rescinded and re-offered to the framework after
// re-registration.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FaultToleranceTest, FrameworkReregister)
{
  // NOTE: We do not use `StartMaster()` because we need to access flags later.
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector slaveDetector(master.get()->pid);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector, agentFlags);
  ASSERT_SOME(slave);

  // Wait for slave registration.
  AWAIT_READY(slaveRegisteredMessage);

  // Create a detector for the scheduler driver because we want the
  // spurious leading master change to be known by the scheduler
  // driver only.
  StandaloneMasterDetector schedDetector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  // Pause the clock so that we know the time at which framework
  // (re-)registration should occur.
  Clock::pause();

  driver.start();

  AWAIT_READY(registered);

  // Take `Clock::now` as precise registration time.
  process::Time registerTime = Clock::now();

  // Advance the clock and trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(resourceOffers);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Advance the clock so that the initial registration time and the
  // re-registration time are distinct.
  Clock::advance(Seconds(2));
  process::Time reregisterTime = Clock::now();

  // Simulate a spurious leading master change at the scheduler.
  schedDetector.appoint(master.get()->pid);

  AWAIT_READY(disconnected);
  AWAIT_READY(reregistered);

  // Trigger the allocation and therefore resource offer instantly to
  // avoid blocking the test.
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // The reregistered framework should get offers.
  AWAIT_READY(resourceOffers2);

  // Check that the framework is displayed correctly in the "/state" endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

  EXPECT_EQ(1u, frameworks.values.size());

  JSON::Object framework = frameworks.values.front().as<JSON::Object>();

  EXPECT_TRUE(framework.values["active"].as<JSON::Boolean>().value);
  EXPECT_TRUE(framework.values["connected"].as<JSON::Boolean>().value);
  EXPECT_FALSE(framework.values["recovered"].as<JSON::Boolean>().value);

  // Even with a paused clock, the value of `registered_time` and
  // `reregistered_time` from the state endpoint can differ slightly
  // from the actual start time since the value went through a number
  // of conversions (`double` to `string` to `JSON::Value`).  Since
  // `Clock::now` is a floating point value, the actual maximal
  // possible difference between the real and observed value depends
  // on both the mantissa and the exponent of the compared values; for
  // simplicity we compare with an epsilon of `1` which allows for
  // e.g., changes in the integer part of values close to an integer
  // value.
  EXPECT_NEAR(
      registerTime.secs(),
      framework.values["registered_time"].as<JSON::Number>().as<double>(),
      1);

  ASSERT_NE(0u, framework.values.count("reregistered_time"));
  EXPECT_NEAR(
      reregisterTime.secs(),
      framework.values["reregistered_time"].as<JSON::Number>().as<double>(),
      1);

  driver.stop();
  driver.join();
}


// This test checks that if a non-partition-aware scheduler that is
// disconnected from the master attempts to launch a task, it receives
// a TASK_LOST status update.
TEST_F(FaultToleranceTest, DisconnectedSchedulerLaunchLost)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<FrameworkRegisteredMessage> message =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious master loss event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  TaskInfo task = createTask(offers.get()[0], "sleep 60");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::REASON_MASTER_DISCONNECTED, status->reason());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status->source());

  driver.stop();
  driver.join();
}


// This test checks that if a partition-aware scheduler that is
// disconnected from the master attempts to launch a task, it receives
// a TASK_DROPPED status update.
TEST_F(FaultToleranceTest, DisconnectedSchedulerLaunchDropped)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<FrameworkRegisteredMessage> message =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious master loss event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  TaskInfo task = createTask(offers.get()[0], "sleep 60");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_DROPPED, status->state());
  EXPECT_EQ(TaskStatus::REASON_MASTER_DISCONNECTED, status->reason());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status->source());

  driver.stop();
  driver.join();
}


// This test checks that a failover scheduler gets the
// retried status update.
TEST_F(FaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop the first status update message
  // between master and the scheduler.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(),
                  _,
                  Not(AnyOf(Eq(master.get()->pid), Eq(slave.get()->pid))));

  driver1.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusUpdateMessage);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers.

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered2));

  // Scheduler1 should get an error due to failover.
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  // Scheduler2 should receive retried status updates.
  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureSatisfy(&statusUpdate))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver2.start();

  AWAIT_READY(registered2);

  Clock::pause();

  // Now advance time enough for the reliable timeout
  // to kick in and another status update is sent.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(statusUpdate);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  Clock::resume();
}


// This test was added to ensure MESOS-420 is fixed.
// We need to make sure that the master correctly handles non-terminal
// tasks with exited executors upon framework re-registration. This is
// possible because the ExitedExecutor message can arrive before the
// terminal status update(s) of its task(s).
TEST_F(FaultToleranceTest, ReregisterFrameworkExitedExecutor)
{
  // First we'll start a master and slave, then register a framework
  // so we can launch a task.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector slaveDetector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector, &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  StandaloneMasterDetector schedDetector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  // Wait until TASK_RUNNING of the task is received.
  AWAIT_READY(statusUpdate);

  EXPECT_CALL(sched, disconnected(&driver));

  // Now that the task is launched, we need to induce the following:
  //   1. ExitedExecutorMessage received by the master prior to a
  //      terminal status update for the corresponding task. This
  //      means we need to drop the status update coming from the
  //      slave.
  //   2. Framework re-registration.
  //
  // To achieve this, we need to:
  //   1. Restart the master (the slave / framework will not detect the
  //      new master automatically using the StandaloneMasterDetector).
  //   2. Notify the slave of the new master.
  //   3. Kill the executor.
  //   4. Drop the status update, but allow the ExitedExecutorMessage.
  //   5. Notify the framework of the new master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slaveDetector.appoint(master.get()->pid);

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  // Allow the executor exited message and drop the status update,
  // it's possible for a duplicate update to occur if the status
  // update manager is notified of the new master after the task was
  // killed.
  Future<ExitedExecutorMessage> executorExitedMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);
  DROP_PROTOBUFS(StatusUpdateMessage(), _, _);

  // Now kill the executor.
  containerizer.destroy(frameworkId, DEFAULT_EXECUTOR_ID);

  AWAIT_READY(executorExitedMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Now notify the framework of the new master.
  schedDetector.appoint(master.get()->pid);

  // Ensure framework successfully registers.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


TEST_F(FaultToleranceTest, ForwardStatusUpdateUnknownExecutor)
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

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING of task1.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offer.id(), {task});

  // Wait until TASK_RUNNING of task1 is received.
  AWAIT_READY(statusUpdate);

  // Simulate the slave receiving status update from an unknown
  // (e.g. exited) executor of the given framework.
  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));           // TASK_RUNNING of task2.

  TaskID taskId;
  taskId.set_value("task2");

  StatusUpdate statusUpdate2 = createStatusUpdate(
      frameworkId,
      offer.slave_id(),
      taskId,
      TASK_RUNNING,
      TaskStatus::SOURCE_SLAVE,
      id::UUID::random(),
      "Dummy update");

  process::dispatch(
      slave.get()->pid,
      &Slave::statusUpdate,
      statusUpdate2,
      UPID());

  // Ensure that the scheduler receives task2's update.
  AWAIT_READY(status);
  EXPECT_EQ(taskId, status->task_id());
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(FaultToleranceTest, SchedulerFailoverExecutorToFrameworkMessage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver1.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched2, frameworkMessage(&driver2, _, _, _))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  Future<Nothing> error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&error));

  Future<UpdateFrameworkMessage> updateFrameworkMessage =
    FUTURE_PROTOBUF(UpdateFrameworkMessage(), _, _);

  driver2.start();

  AWAIT_READY(error);

  AWAIT_READY(registered);

  // Wait for the slave to get the updated framework pid.
  AWAIT_READY(updateFrameworkMessage);

  execDriver->sendFrameworkMessage("Executor to Framework message");

  AWAIT_READY(frameworkMessage);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();
}


TEST_F(FaultToleranceTest, SchedulerFailoverFrameworkToExecutorMessage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  driver1.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&error));

  driver2.start();

  AWAIT_READY(error);

  AWAIT_READY(registered);

  Future<string> frameworkMessage;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&frameworkMessage));

  // Since 'sched2' doesn't receive any offers the framework message
  // should go through the master.
  Future<mesos::scheduler::Call> messageCall = FUTURE_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::MESSAGE,
      _,
      master.get()->pid);

  driver2.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello world");

  AWAIT_READY(messageCall);

  AWAIT_EQ("hello world", frameworkMessage);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();
}


// This test verifies that a partitioned framework that still
// thinks it is registered with the master cannot kill a task because
// the master has reregistered another instance of the framework.
// What this test does:
// 1. Launch a master, slave and scheduler.
// 2. Scheduler launches a task.
// 3. Launch a second failed over scheduler.
// 4. Make the first scheduler believe it is still registered.
// 5. First scheduler attempts to kill the task which is ignored by the master.
TEST_F(FaultToleranceTest, IgnoreKillTaskFromUnregisteredFramework)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Start the first scheduler and launch a task.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  Future<mesos::scheduler::Call> acknowledgeCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACKNOWLEDGE, _, _);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver1.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Wait for the status update acknowledgement to be sent. This
  // ensures the slave doesn't resend the TASK_RUNNING update to the
  // failed over scheduler (below).
  AWAIT_READY(acknowledgeCall);

  // Now start the second failed over scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return()); // Ignore any offers.

  // Drop the framework error message from the master to simulate
  // a partitioned framework.
  Future<FrameworkErrorMessage> frameworkErrorMessage =
    DROP_PROTOBUF(FrameworkErrorMessage(), _, _);

  driver2.start();

  AWAIT_READY(frameworkErrorMessage);

  AWAIT_READY(registered);

  // Now both the frameworks think they are registered with the
  // master, but the master only knows about the second framework.

  // A 'killTask' by first framework should be dropped by the master.
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  // 'TASK_FINSIHED' by the executor should reach the second framework.
  Future<TaskStatus> status2;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<mesos::scheduler::Call> killCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::KILL, _, _);

  driver1.killTask(status->task_id());

  AWAIT_READY(killCall);

  // By this point the master must have processed and ignored the
  // 'killTask' message from the first framework. To verify this,
  // the executor sends 'TASK_FINISHED' to ensure the only update
  // received by the scheduler is 'TASK_FINISHED' and not
  // 'TASK_KILLED'.
  TaskStatus finishedStatus;
  finishedStatus = status.get();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();
}


// This test checks that a scheduler exit shuts down the executor.
TEST_F(FaultToleranceTest, SchedulerExit)
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

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  AWAIT_READY(offers);

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  // Ensure that the executor receives a shutdown message after the
  // scheduler exit.
  AWAIT_READY(shutdown);
}


TEST_F(FaultToleranceTest, SlaveReliableRegistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the first slave registered message, allow subsequent messages.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    DROP_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(slaveRegisteredMessage);

  // Trigger the registration retry instantly to avoid blocking the test.
  Clock::pause();
  Clock::advance(DEFAULT_REGISTRATION_BACKOFF_FACTOR);

  AWAIT_READY(resourceOffers);

  driver.stop();
  driver.join();

  Clock::resume();
}


TEST_F(FaultToleranceTest, SlaveReregisterOnZKExpiration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(resourceOffers);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave.
  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();
}


// This test ensures that a master properly handles the
// re-registration of a framework when an empty executor is present
// on a slave. This was added to prevent regressions on MESOS-1821.
TEST_F(FaultToleranceTest, FrameworkReregisterEmptyExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector slaveDetector(master.get()->pid);
  StandaloneMasterDetector schedulerDetector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector, &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_FINISHED, status->state());

  // Make sure the acknowledgement is processed the slave.
  AWAIT_READY(_statusUpdateAcknowledgement);

  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(Return());

  // Now we bring up a new master, the executor from the slave
  // re-registration will be empty (no tasks).
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(
        SlaveReregisteredMessage(), master.get()->pid, slave.get()->pid);

  // Re-register the slave.
  slaveDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Re-register the framework.
  schedulerDetector.appoint(master.get()->pid);

  AWAIT_READY(registered);

  Future<ExitedExecutorMessage> executorExitedMessage =
    FUTURE_PROTOBUF(
        ExitedExecutorMessage(), slave.get()->pid, master.get()->pid);

  // Now kill the executor.
  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  // Ensure the master correctly handles the exited executor
  // with no tasks!
  AWAIT_READY(executorExitedMessage);
  AWAIT_READY(executorLost);

  driver.stop();
  driver.join();
}


// This test ensures that if a master incorrectly thinks that it is
// leading, the scheduler driver will drop messages from this master.
// Unfortunately, it is not currently possible to start more than one
// master within the same process. So, this test merely simulates this
// by spoofing messages.
// This test does the following:
//   1. Start a master, scheduler, launch a task.
//   2. Spoof a lost task message for the slave.
//   3. Once the message is sent to the scheduler, kill the task.
//   4. Ensure the task was KILLED rather than LOST.
TEST_F(FaultToleranceTest, SplitBrainMasters)
{
  // 1. Start a master, scheduler, and launch a task.
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

  Future<Message> registered =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(frameworkId);
  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  // 2. Spoof a lost task message for the slave.
  StatusUpdateMessage lostUpdate;
  lostUpdate.mutable_update()->CopyFrom(createStatusUpdate(
      frameworkId.get(),
      runningStatus->slave_id(),
      runningStatus->task_id(),
      TASK_LOST,
      TaskStatus::SOURCE_SLAVE,
      id::UUID::random()));

  // Spoof a message from a random master; this should be dropped by
  // the scheduler driver. Since this is delivered locally, it is
  // synchronously placed on the scheduler driver's queue.
  process::post(UPID("master2@127.0.0.1:50"), registered->to, lostUpdate);

  // 3. Once the message is sent to the scheduler, kill the task.
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killedStatus));

  driver.killTask(runningStatus->task_id());

  // 4. Ensure the task was KILLED rather than LOST.
  AWAIT_READY(killedStatus);
  EXPECT_EQ(TASK_KILLED, killedStatus->state());

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Return());

  driver.stop();
  driver.join();
}


// This test verifies that when a framework tries to change its
// principal on re-registration, it gets an error.
TEST_F(FaultToleranceTest, ChangePrincipalOnReRegistrationFails)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();
  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler with ANOTHER principal
  // using the framework id recorded from the first scheduler, and wait
  // until it gets an error.

  FrameworkInfo finfo2 = DEFAULT_FRAMEWORK_INFO;

  finfo2.mutable_id()->MergeFrom(frameworkId.get());
  finfo2.set_principal(DEFAULT_CREDENTIAL_2.principal());

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, finfo2, master.get()->pid, DEFAULT_CREDENTIAL_2);

  // Scheduler should never be registered.
  // We cannot check "never", therefore we check that it is not registered
  // at least before it receives the error.

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(AtMost(0));

  Future<Nothing> sched2Error;
  EXPECT_CALL(sched2,
      error(&driver2, "Changing framework's principal is not allowed."))
    .WillOnce(FutureSatisfy(&sched2Error));

  driver2.start();
  AWAIT_READY(sched2Error);
}


// This test verifies that when a framework reregisters with updated
// FrameworkInfo, it gets updated in the master. The steps involved
// are:
//   1. Launch a master, slave and scheduler.
//   2. Record FrameworkID of launched scheduler.
//   3. Launch a second scheduler which has the same FrameworkID as
//      the first scheduler and also has updated FrameworkInfo.
//   4. Verify that the state of the master is updated with the new
//      FrameworkInfo object.
TEST_F(FaultToleranceTest, UpdateFrameworkInfoOnSchedulerFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler with updated information.

  FrameworkInfo finfo1 = DEFAULT_FRAMEWORK_INFO;
  finfo1.clear_capabilities();
  finfo1.clear_roles();

  finfo1.set_name("Framework 1");
  finfo1.set_failover_timeout(1000);
  finfo1.mutable_labels()->add_labels()->CopyFrom(createLabel("foo", "bar"));

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, finfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler, along with the
  // updated FrameworkInfo and wait until it gets a registered
  // callback.

  FrameworkInfo finfo2 = finfo1;

  finfo2.mutable_id()->MergeFrom(frameworkId.get());
  auto capabilityType = FrameworkInfo::Capability::REVOCABLE_RESOURCES;
  finfo2.add_capabilities()->set_type(capabilityType);
  finfo2.set_name("Framework 2");
  finfo2.set_webui_url("http://localhost:8080/");
  finfo2.set_failover_timeout(100);
  finfo2.set_hostname("myHostname");
  finfo2.clear_labels();
  finfo2.mutable_labels()->add_labels()->CopyFrom(createLabel("baz", "qux"));

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, finfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  // Scheduler1's expectations.
  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();
  EXPECT_EQ(1u, state.values.count("frameworks"));
  JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
  EXPECT_EQ(1u, frameworks.values.size());
  JSON::Object framework = frameworks.values.front().as<JSON::Object>();

  EXPECT_EQ(1u, framework.values.count("name"));
  JSON::String name = framework.values["name"].as<JSON::String>();
  EXPECT_EQ(finfo2.name(), name.value);

  EXPECT_EQ(1u, framework.values.count("webui_url"));
  JSON::String webuiUrl = framework.values["webui_url"].as<JSON::String>();
  EXPECT_EQ(finfo2.webui_url(), webuiUrl.value);

  EXPECT_EQ(1u, framework.values.count("failover_timeout"));
  JSON::Number failoverTimeout =
    framework.values["failover_timeout"].as<JSON::Number>();
  EXPECT_EQ(finfo2.failover_timeout(), failoverTimeout.as<double>());

  EXPECT_EQ(1u, framework.values.count("hostname"));
  JSON::String hostname = framework.values["hostname"].as<JSON::String>();
  EXPECT_EQ(finfo2.hostname(), hostname.value);

  EXPECT_EQ(1u, framework.values.count("capabilities"));
  JSON::Array capabilities =
    framework.values["capabilities"].as<JSON::Array>();
  EXPECT_EQ(1u, capabilities.values.size());
  JSON::String capability = capabilities.values.front().as<JSON::String>();
  EXPECT_EQ(FrameworkInfo::Capability::Type_Name(capabilityType),
            capability.value);

  EXPECT_EQ(1u, framework.values.count("labels"));
  JSON::Array labels = framework.values["labels"].as<JSON::Array>();

  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("baz", "qux"))),
      labels.values[0]);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());
}


// This test verifies that when a framework reregisters after master
// failover with an updated FrameworkInfo, the updated FrameworkInfo
// is reflected in the master.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    FaultToleranceTest,
    UpdateFrameworkInfoOnMasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("Framework 1");
  frameworkInfo1.set_failover_timeout(1000);
  frameworkInfo1.set_checkpoint(true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver1.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  AWAIT_READY(statusUpdateAck);

  // Stop the master.
  master->reset();

  // While the master is down, shutdown the first framework.
  driver1.stop();
  driver1.join();

  // Restart master; ensure the agent reregisters before the second
  // scheduler connects. This ensures the first framework's
  // FrameworkInfo is recovered from the reregistering agent.
  Future<SlaveReregisteredMessage> slaveReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), _, slave.get()->pid);

  master = StartMaster();
  ASSERT_SOME(master);

  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregistered);

  // Connect a new scheduler that uses the same framework ID but a
  // different `FrameworkInfo`. Note that updating the `checkpoint`
  // field is NOT supported, so we expect the master to ignore the
  // updated version of this field.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.mutable_id()->MergeFrom(frameworkId.get());
  frameworkInfo2.set_name("Framework 2");
  frameworkInfo2.set_failover_timeout(2000);
  frameworkInfo2.set_checkpoint(false);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  driver2.start();

  AWAIT_READY(sched2Registered);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();
  ASSERT_EQ(1u, frameworks.values.size());

  JSON::Object framework = frameworks.values.front().as<JSON::Object>();
  EXPECT_EQ(frameworkId.get(), framework.values["id"].as<JSON::String>().value);
  EXPECT_EQ("Framework 2", framework.values["name"].as<JSON::String>().value);
  EXPECT_EQ(2000, framework.values["failover_timeout"].as<JSON::Number>());
  EXPECT_TRUE(framework.values["checkpoint"].as<JSON::Boolean>().value);

  driver2.stop();
  driver2.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
