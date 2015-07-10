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

#include <gmock/gmock.h>

#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/message.hpp>
#include <process/pid.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::scheduler::Event;

using process::Future;
using process::Message;
using process::PID;
using process::UPID;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace tests {


// These tests intercept master messages and manually
// post Event messages to the driver.
class SchedulerDriverEventTest : public MesosTest {};


// Ensures that the driver can handle the RESCIND event.
TEST_F(SchedulerDriverEventTest, Rescind)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

  Event event;
  event.set_type(Event::RESCIND);
  event.mutable_rescind()->mutable_offer_id()->set_value("O");

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, event.rescind().offer_id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(offerRescinded);
}


// Ensures the scheduler driver can handle the UPDATE event.
TEST_F(SchedulerDriverEventTest, Update)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage.get().body));

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

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(statusUpdate);

  // Generate an update that requires acknowledgement.
  event.mutable_update()->mutable_status()->set_uuid(UUID::random().toBytes());

  Future<mesos::scheduler::Call> acknowledgement = DROP_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACKNOWLEDGE, _, _);

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(statusUpdate2);
  AWAIT_READY(acknowledgement);
}


// Ensures that the driver can handle the MESSAGE event.
TEST_F(SchedulerDriverEventTest, Message)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

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

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(frameworkMessage);
}


// Ensures that the driver can handle the FAILURE event.
TEST_F(SchedulerDriverEventTest, Failure)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

  // Send a failure for an executor, this should be dropped
  // to match the existing behavior of the scheduler driver.
  SlaveID slaveId;
  slaveId.set_value("S");

  Event event;
  event.set_type(Event::FAILURE);
  event.mutable_failure()->mutable_slave_id()->CopyFrom(slaveId);
  event.mutable_failure()->mutable_executor_id()->set_value("E");

  process::post(master.get(), frameworkPid, event);

  // Now, post a failure for a slave and expect a 'slaveLost'.
  event.mutable_failure()->clear_executor_id();

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, slaveId))
    .WillOnce(FutureSatisfy(&slaveLost));

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(slaveLost);
}


// Ensures that the driver can handle the ERROR event.
TEST_F(SchedulerDriverEventTest, Error)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

  Event event;
  event.set_type(Event::ERROR);
  event.mutable_error()->set_message("error message");

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, event.error().message()))
    .WillOnce(FutureSatisfy(&error));

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(error);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
