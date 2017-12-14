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

#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/message.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::scheduler::Event;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::UPID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Eq;

namespace mesos {
namespace internal {
namespace tests {


// These tests intercept master messages and manually
// post Event messages to the driver.
class SchedulerDriverEventTest : public MesosTest {};


// Ensures that the driver can handle the SUBSCRIBED event.
TEST_F(SchedulerDriverEventTest, Subscribed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Make sure the initial registration calls 'registered'.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Intercept the registration message, send a SUBSCRIBED instead.
  Future<Message> frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  // Ensure that there will be no (re-)registration retries
  // from the scheduler driver.
  Clock::pause();

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_framework_id()->CopyFrom(frameworkId);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the SUBSCRIBED event
// after a disconnection with the master.
TEST_F(SchedulerDriverEventTest, SubscribedDisconnection)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Make sure the initial registration calls 'registered'.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  // Intercept the registration message, send a SUBSCRIBED instead.
  Future<Message> frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  // Ensure that there will be no (re-)registration retries
  // from the scheduler driver.
  Clock::pause();

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_framework_id()->CopyFrom(frameworkId);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(registered);

  // Simulate a disconnection and expect a 'reregistered' call.
  EXPECT_CALL(sched, disconnected(&driver));

  Future<Message> frameworkReregisteredMessage =
    DROP_MESSAGE(Eq(FrameworkReregisteredMessage().GetTypeName()), _, _);

  detector.appoint(master.get()->pid);

  AWAIT_READY(frameworkReregisteredMessage);

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(reregistered);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the SUBSCRIBED event
// after a master failover.
TEST_F(SchedulerDriverEventTest, SubscribedMasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Make sure the initial registration calls 'registered'.
  MockScheduler sched;
  StandaloneMasterDetector detector(master.get()->pid);
  TestingMesosSchedulerDriver driver(&sched, &detector, frameworkInfo);

  // Intercept the registration message, send a SUBSCRIBED instead.
  Future<Message> frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  // Ensure that there will be no (re-)registration retries
  // from the scheduler driver.
  Clock::pause();

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_framework_id()->CopyFrom(frameworkId);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(registered);

  EXPECT_CALL(sched, disconnected(&driver));

  // Fail over the master and expect a 'reregistered' call.
  // Note that the master sends a registered message for
  // this case (see MESOS-786).
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  detector.appoint(master.get()->pid);

  AWAIT_READY(frameworkRegisteredMessage);

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(reregistered);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the SUBSCRIBED event
// after a scheduler failover.
TEST_F(SchedulerDriverEventTest, SubscribedSchedulerFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  // Make sure the initial registration calls 'registered'.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // Intercept the registration message, send a SUBSCRIBED instead.
  Future<Message> frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  // Ensure that there will be no (re-)registration retries
  // from the scheduler driver.
  Clock::pause();

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_framework_id()->CopyFrom(frameworkId);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(registered);

  // Fail over the scheduler and expect a 'registered' call.
  driver.stop(true);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  frameworkRegisteredMessage =
    DROP_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver2.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid2 = frameworkRegisteredMessage->to;

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered2));

  process::post(master.get()->pid, frameworkPid2, event);

  AWAIT_READY(registered2);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle an OFFERS event.
// Note that this includes the ability to bypass the
// master when sending framework messages.
TEST_F(SchedulerDriverEventTest, Offers)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  schedDriver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  // Start a slave and capture the offers.
  Future<ResourceOffersMessage> resourceOffersMessage =
    DROP_PROTOBUF(ResourceOffersMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  AWAIT_READY(resourceOffersMessage);

  google::protobuf::RepeatedPtrField<Offer> offers =
    resourceOffersMessage->offers();

  ASSERT_EQ(1, offers.size());

  // Ignore future offer messages.
  DROP_PROTOBUFS(ResourceOffersMessage(), _, _);

  // Send the offers event and expect a 'resourceOffers' call.
  Event event;
  event.set_type(Event::OFFERS);
  event.mutable_offers()->mutable_offers()->CopyFrom(offers);

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(resourceOffers);

  // To test that the framework -> executor messages are
  // sent directly to the slave, launch a task and send
  // the executor a message.
  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers.Get(0), "", DEFAULT_EXECUTOR_ID);

  schedDriver.launchTasks(offers.Get(0).id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // This message should skip the master!
  Future<FrameworkToExecutorMessage> frameworkToExecutorMessage =
    FUTURE_PROTOBUF(
        FrameworkToExecutorMessage(),
        frameworkPid,
        slave.get()->pid);

  Future<string> data;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&data));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.Get(0).slave_id(), "hello");

  AWAIT_READY(frameworkToExecutorMessage);
  AWAIT_EXPECT_EQ("hello", data);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();
}


// Ensures that the driver can handle the RESCIND event.
TEST_F(SchedulerDriverEventTest, Rescind)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  Event event;
  event.set_type(Event::RESCIND);
  event.mutable_rescind()->mutable_offer_id()->set_value("O");

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, event.rescind().offer_id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(offerRescinded);

  driver.stop();
  driver.join();
}


// Ensures the scheduler driver can handle the UPDATE event.
TEST_F(SchedulerDriverEventTest, Update)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();

  SlaveID slaveId;
  slaveId.set_value("S");

  TaskID taskId;
  taskId.set_value("T");

  ExecutorID executorId;
  executorId.set_value("E");

  // Generate an update that needs no acknowledgement.
  Event event;
  event.set_type(Event::UPDATE);
  event.mutable_update()->mutable_status()->CopyFrom(
      protobuf::createStatusUpdate(
          frameworkId,
          slaveId,
          taskId,
          TASK_RUNNING,
          TaskStatus::SOURCE_MASTER,
          None(),
          "message",
          None(),
          executorId).status());

  Future<Nothing> statusUpdate;
  Future<Nothing> statusUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, event.update().status()))
    .WillOnce(FutureSatisfy(&statusUpdate))
    .WillOnce(FutureSatisfy(&statusUpdate2));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(statusUpdate);

  // Generate an update that requires acknowledgement.
  event.mutable_update()->mutable_status()->set_uuid(
      id::UUID::random().toBytes());

  Future<mesos::scheduler::Call> acknowledgement = DROP_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACKNOWLEDGE, _, _);

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(statusUpdate2);
  AWAIT_READY(acknowledgement);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the MESSAGE event.
TEST_F(SchedulerDriverEventTest, Message)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  Event event;
  event.set_type(Event::MESSAGE);
  event.mutable_message()->mutable_slave_id()->set_value("S");
  event.mutable_message()->mutable_executor_id()->set_value("E");
  event.mutable_message()->set_data("data");

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched, frameworkMessage(
      &driver,
      event.message().executor_id(),
      event.message().slave_id(),
      event.message().data()))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(frameworkMessage);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the FAILURE event.
TEST_F(SchedulerDriverEventTest, Failure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  // Send a failure for an executor, which should trigger executorLost callback.
  SlaveID slaveId;
  slaveId.set_value("S");

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;

  const int32_t status = 255;

  Event event;
  event.set_type(Event::FAILURE);
  event.mutable_failure()->mutable_slave_id()->CopyFrom(slaveId);
  event.mutable_failure()->mutable_executor_id()->CopyFrom(executorId);
  event.mutable_failure()->set_status(status);

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, executorId, slaveId, status))
    .WillOnce(FutureSatisfy(&executorLost));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(executorLost);

  // Now, post a failure for a slave and expect a 'slaveLost'.
  event.mutable_failure()->clear_executor_id();

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, slaveId))
    .WillOnce(FutureSatisfy(&slaveLost));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();
}


// Ensures that the driver can handle the ERROR event.
TEST_F(SchedulerDriverEventTest, Error)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  Event event;
  event.set_type(Event::ERROR);
  event.mutable_error()->set_message("error message");

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, event.error().message()))
    .WillOnce(FutureSatisfy(&error));

  process::post(master.get()->pid, frameworkPid, event);

  AWAIT_READY(error);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
