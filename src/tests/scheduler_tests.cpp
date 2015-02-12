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

#include <queue>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/queue.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::tests;

using mesos::master::Master;

using mesos::slave::Containerizer;
using mesos::slave::Slave;

using mesos::scheduler::Call;
using mesos::scheduler::Event;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Promise;
using process::Queue;

using process::http::OK;

using process::metrics::internal::MetricsProcess;

using std::queue;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Return;


class SchedulerTest : public MesosTest
{
protected:
  // Helper class for using EXPECT_CALL since the Mesos scheduler API
  // is callback based.
  class Callbacks
  {
  public:
    MOCK_METHOD0(connected, void(void));
    MOCK_METHOD0(disconnected, void(void));
    MOCK_METHOD1(received, void(const std::queue<Event>&));
  };
};


// Enqueues all received events into a libprocess queue.
ACTION_P(Enqueue, queue)
{
  std::queue<Event> events = arg0;
  while (!events.empty()) {
    queue->put(events.front());
    events.pop();
  }
}


TEST_F(SchedulerTest, TaskRunning)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  Callbacks callbacks;

  Future<Nothing> connected;
  EXPECT_CALL(callbacks, connected())
    .WillOnce(FutureSatisfy(&connected));

  scheduler::Mesos mesos(
      master.get(),
      DEFAULT_CREDENTIAL,
      lambda::bind(&Callbacks::connected, lambda::ref(callbacks)),
      lambda::bind(&Callbacks::disconnected, lambda::ref(callbacks)),
      lambda::bind(&Callbacks::received, lambda::ref(callbacks), lambda::_1));

  AWAIT_READY(connected);

  Queue<Event> events;

  EXPECT_CALL(callbacks, received(_))
    .WillRepeatedly(Enqueue(&events));

  {
    Call call;
    call.mutable_framework_info()->CopyFrom(DEFAULT_FRAMEWORK_INFO);
    call.set_type(Call::REGISTER);

    mesos.send(call);
  }

  Future<Event> event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::REGISTERED, event.get().type());

  FrameworkID id(event.get().registered().framework_id());

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_NE(0, event.get().offers().offers().size());

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer, update(_, _))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())))
    .WillRepeatedly(Return(Future<Nothing>())); // Ignore subsequent calls.

  TaskInfo taskInfo;
  taskInfo.set_name("");
  taskInfo.mutable_task_id()->set_value("1");
  taskInfo.mutable_slave_id()->CopyFrom(
      event.get().offers().offers(0).slave_id());
  taskInfo.mutable_resources()->CopyFrom(
      event.get().offers().offers(0).resources());
  taskInfo.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // TODO(benh): Enable just running a task with a command in the tests:
  //   taskInfo.mutable_command()->set_value("sleep 10");

  {
    Call call;
    call.mutable_framework_info()->CopyFrom(DEFAULT_FRAMEWORK_INFO);
    call.mutable_framework_info()->mutable_id()->CopyFrom(id);
    call.set_type(Call::LAUNCH);
    call.mutable_launch()->add_task_infos()->CopyFrom(taskInfo);
    call.mutable_launch()->add_offer_ids()->CopyFrom(
        event.get().offers().offers(0).id());

    mesos.send(call);
  }

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::UPDATE, event.get().type());
  EXPECT_EQ(TASK_RUNNING, event.get().update().status().state());

  AWAIT_READY(update);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// TODO(benh): Write test for sending Call::Acknowledgement through
// master to slave when Event::Update was generated locally.


class MesosSchedulerDriverTest : public MesosTest {};


TEST_F(MesosSchedulerDriverTest, MetricsEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(registered);

  Future<process::http::Response> response =
    process::http::get(MetricsProcess::instance()->self(), "/snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object metrics = parse.get();

  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_messages"));
  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_dispatches"));

  driver.stop();
  driver.join();

  Shutdown();
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
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

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
  EXPECT_NO_FUTURE_PROTOBUFS(
      StatusUpdateAcknowledgementMessage(), _ , master.get());

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

  Shutdown();
}
