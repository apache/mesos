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

#include <gmock/gmock.h>

#include <mesos/executor.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/owned.hpp>

#include <stout/lambda.hpp>
#include <stout/try.hpp>

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Future;
using process::PID;
using process::Owned;

using std::string;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

// NOTE: These tests are for the v1 HTTP API and are semantically similar to
// the corresponding tests in `src/tests/fault_tolerance_tests.cpp`.
class HttpFaultToleranceTest : public MesosTest {};

// This test verifies that a framework attempting to resubscribe
// with a different principal during its failover timeout
// gets an error.
TEST_F(HttpFaultToleranceTest, FrameworkPrincipalChangeFails)
{
  master::Flags flags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  v1::FrameworkID frameworkId;

  // Launch the first (i.e., failing) scheduler and wait until it receives
  // a `SUBSCRIBED` event to launch the second (i.e., failover) scheduler.
  {
    v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_failover_timeout(Weeks(2).secs());

    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    Future<Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::JSON,
      scheduler);

    AWAIT_READY(subscribed);

    frameworkId = subscribed->framework_id();
  }


  // Now launch the second (i.e., failover) scheduler using the framework id
  // recorded from the first scheduler but another set of valid credentials.
  // The scheduler should get an error instead of "Subscribed" message.
  // The master should disconnect the scheduler after sending an error.
  {
    v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

    frameworkInfo.mutable_id()->CopyFrom(frameworkId);
    frameworkInfo.set_principal(v1::DEFAULT_CREDENTIAL_2.principal());

    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    // We do not resubscribe after the master disconnects us.
    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo, frameworkId))
      .WillRepeatedly(Return());

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    // Fail the test if we got `Subscribed` before the master disconnects us.
    Future<Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .Times(AtMost(0));

    Future<Nothing> disconnected;
    EXPECT_CALL(*scheduler, disconnected(_))
      .WillOnce(FutureSatisfy(&disconnected));

    Future<Event::Error> error;
    EXPECT_CALL(*scheduler, error(_, _))
      .WillOnce(FutureArg<1>(&error));

    v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::JSON,
      scheduler,
      None(),
      v1::DEFAULT_CREDENTIAL_2);

    AWAIT_READY(error);
    EXPECT_EQ(error->message(),
              "Changing framework's principal is not allowed.");

    AWAIT_READY(disconnected);
  }
}


// This test verifies that a framework attempting to subscribe
// after its failover timeout has elapsed is disallowed.
TEST_F(HttpFaultToleranceTest, SchedulerSubscribeAfterFailoverTimeout)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<Nothing> deactivateFramework = FUTURE_DISPATCH(
      _, &master::allocator::MesosAllocatorProcess::deactivateFramework);

  v1::FrameworkID frameworkId;

  ContentType contentType = ContentType::PROTOBUF;

  // Launch the first (i.e., failing) scheduler and wait until it receives
  // a `SUBSCRIBED` event to launch the second (i.e., failover) scheduler.
  {
    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(FutureSatisfy(&connected));

    v1::scheduler::TestMesos schedulerLibrary(
        master.get()->pid,
        contentType,
        scheduler);

    AWAIT_READY(connected);

    Future<Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    {
      Call call;
      call.set_type(Call::SUBSCRIBE);
      Call::Subscribe* subscribe = call.mutable_subscribe();
      subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

      schedulerLibrary.send(call);
    }

    AWAIT_READY(subscribed);

    frameworkId = subscribed->framework_id();
  }

  // Wait until master schedules the framework for removal.
  AWAIT_READY(deactivateFramework);

  // Simulate framework failover timeout.
  Clock::pause();
  Clock::settle();

  Try<Duration> failoverTimeout =
    Duration::create(frameworkInfo.failover_timeout());

  ASSERT_SOME(failoverTimeout);

  Future<Nothing> frameworkFailoverTimeout =
    FUTURE_DISPATCH(_, &Master::frameworkFailoverTimeout);

  Clock::advance(failoverTimeout.get());
  Clock::resume();

  // Wait until master actually marks the framework as completed.
  AWAIT_READY(frameworkFailoverTimeout);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  {
    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(FutureSatisfy(&connected))
      .WillRepeatedly(Return()); // Ignore future invocations.

    v1::scheduler::TestMesos schedulerLibrary(
        master.get()->pid,
        contentType,
        scheduler);

    AWAIT_READY(connected);

    // Framework should get `Error` event because the framework with this id
    // is marked as completed.
    Future<Nothing> error;
    EXPECT_CALL(*scheduler, error(_, _))
      .WillOnce(FutureSatisfy(&error));

    EXPECT_CALL(*scheduler, disconnected(_))
      .Times(AtMost(1));

    {
      Call call;
      call.mutable_framework_id()->CopyFrom(frameworkId);
      call.set_type(Call::SUBSCRIBE);

      Call::Subscribe* subscribe = call.mutable_subscribe();
      subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
      subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

      schedulerLibrary.send(call);
    }

    AWAIT_READY(error);
  }
}


// This test verifies that a framework attempting to subscribe after teardown
// is disallowed.
TEST_F(HttpFaultToleranceTest, SchedulerSubscribeAfterTeardown)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  v1::FrameworkID frameworkId;

  ContentType contentType = ContentType::PROTOBUF;

  // Launch the first (i.e., failing) scheduler and wait until it receives
  // a `SUBSCRIBED` event to launch the second (i.e., failover) scheduler.
  {
    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(FutureSatisfy(&connected))
      .WillRepeatedly(Return()); // Ignore future invocations.

    v1::scheduler::TestMesos schedulerLibrary(
        master.get()->pid,
        contentType,
        scheduler);

    AWAIT_READY(connected);

    Future<Event::Subscribed> subscribed;
    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    EXPECT_CALL(*scheduler, heartbeat(_))
      .WillRepeatedly(Return()); // Ignore heartbeats.

    {
      Call call;
      call.set_type(Call::SUBSCRIBE);

      Call::Subscribe* subscribe = call.mutable_subscribe();
      subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

      schedulerLibrary.send(call);
    }

    AWAIT_READY(subscribed);

    frameworkId = subscribed->framework_id();

    Future<Nothing> removeFramework = FUTURE_DISPATCH(
      _, &master::allocator::MesosAllocatorProcess::removeFramework);

    Future<Nothing> disconnected;
    EXPECT_CALL(*scheduler, disconnected(_))
      .WillOnce(FutureSatisfy(&disconnected));

    // Teardown the scheduler now.
    {
      Call call;
      call.mutable_framework_id()->CopyFrom(frameworkId);
      call.set_type(Call::TEARDOWN);

      schedulerLibrary.send(call);
    }

    // Wait until master actually marks the framework as completed.
    AWAIT_READY(removeFramework);

    // Wait for `removeFramework()` to be completed on the master.
    Clock::pause();
    Clock::settle();
    Clock::resume();

    // The scheduler should eventually realize the disconnection.
    AWAIT_READY(disconnected);
  }

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  {
    auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

    Future<Nothing> connected;
    EXPECT_CALL(*scheduler, connected(_))
      .WillOnce(FutureSatisfy(&connected))
      .WillRepeatedly(Return()); // Ignore future invocations.

    v1::scheduler::TestMesos schedulerLibrary(
        master.get()->pid,
        contentType,
        scheduler);

    AWAIT_READY(connected);

    // Framework should get `Error` event because the framework
    // with this id is marked as completed.
    Future<Nothing> error;
    EXPECT_CALL(*scheduler, error(_, _))
      .WillOnce(FutureSatisfy(&error));

    EXPECT_CALL(*scheduler, disconnected(_))
      .Times(AtMost(1));

    {
      Call call;
      call.mutable_framework_id()->CopyFrom(frameworkId);
      call.set_type(Call::SUBSCRIBE);

      Call::Subscribe* subscribe = call.mutable_subscribe();
      subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
      subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

      schedulerLibrary.send(call);
    }

    AWAIT_READY(error);
  }
}


// This test checks that a failed over scheduler gets the retried status update
// when the original instance dies without acknowledging the update.
TEST_F(HttpFaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos schedulerLibrary(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged));

  Future<Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", executorId));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update);

  EXPECT_EQ(v1::TASK_RUNNING, update->status().state());
  EXPECT_EQ(executorId, devolve(update->status().executor_id()));

  EXPECT_TRUE(update->status().has_executor_id());
  EXPECT_TRUE(update->status().has_uuid());

  // Failover the scheduler without acknowledging the status update.

  auto scheduler2 = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected2;
  EXPECT_CALL(*scheduler2, connected(_))
    .WillOnce(FutureSatisfy(&connected2));

  // Failover to another scheduler instance.
  v1::scheduler::TestMesos schedulerLibrary2(
      master.get()->pid,
      contentType,
      scheduler2);

  AWAIT_READY(connected2);

  // The previously connected scheduler instance should receive an
  // error/disconnected event.
  Future<Nothing> error;
  EXPECT_CALL(*scheduler, error(_, _))
    .WillOnce(FutureSatisfy(&error));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  EXPECT_CALL(*scheduler2, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler2, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Scheduler2 should receive the retried status update.
  Future<Nothing> update2;
  EXPECT_CALL(*scheduler2, update(_, _))
    .WillOnce(FutureSatisfy(&update2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    schedulerLibrary2.send(call);
  }

  AWAIT_READY(error);
  AWAIT_READY(disconnected);
  AWAIT_READY(subscribed);

  EXPECT_EQ(frameworkId, subscribed->framework_id());

  Clock::pause();

  // Now advance time enough for the reliable timeout to kick in and
  // another status update to be sent.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(update2);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// This test ensures that the failed over scheduler receives the executor to
// framework message.
TEST_F(HttpFaultToleranceTest, SchedulerFailoverExecutorToFrameworkMessage)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos schedulerLibrary(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  v1::executor::Mesos* executorLib;
  EXPECT_CALL(*executor, subscribed(_, _))
    .WillOnce(SaveArg<0>(&executorLib));

  Future<Nothing> launch;
  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(FutureSatisfy(&launch));

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", executorId));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(launch);

  auto scheduler2 = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected2;
  EXPECT_CALL(*scheduler2, connected(_))
    .WillOnce(FutureSatisfy(&connected2));

  // Failover to another scheduler instance.
  v1::scheduler::TestMesos schedulerLibrary2(
      master.get()->pid,
      contentType,
      scheduler2);

  AWAIT_READY(connected2);

  // The previously connected scheduler instance should receive an
  // error/disconnected event.
  Future<Nothing> error;
  EXPECT_CALL(*scheduler, error(_, _))
    .WillOnce(FutureSatisfy(&error));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  EXPECT_CALL(*scheduler2, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler2, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    schedulerLibrary2.send(call);
  }

  AWAIT_READY(error);
  AWAIT_READY(disconnected);
  AWAIT_READY(subscribed);

  EXPECT_EQ(frameworkId, subscribed->framework_id());

  Future<Event::Message> message;
  EXPECT_CALL(*scheduler2, message(_, _))
    .WillOnce(FutureArg<1>(&message));

  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(evolve(executorId));

    call.set_type(v1::executor::Call::MESSAGE);

    v1::executor::Call::Message* message = call.mutable_message();
    message->set_data("hello world");

    executorLib->send(call);
  }

  AWAIT_READY(message);
  ASSERT_EQ("hello world", message->data());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// This test ensures that the failed over scheduler is able to send a message
// to the executor.
TEST_F(HttpFaultToleranceTest, SchedulerFailoverFrameworkToExecutorMessage)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos schedulerLibrary(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  Future<Nothing> launch;
  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(FutureSatisfy(&launch));

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", executorId));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(launch);

  auto scheduler2 = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected2;
  EXPECT_CALL(*scheduler2, connected(_))
    .WillOnce(FutureSatisfy(&connected2));

  // Failover to another scheduler instance.
  v1::scheduler::TestMesos schedulerLibrary2(
      master.get()->pid,
      contentType,
      scheduler2);

  AWAIT_READY(connected2);

  // The previously connected scheduler instance should receive an
  // error/disconnected event.
  Future<Nothing> error;
  EXPECT_CALL(*scheduler, error(_, _))
    .WillOnce(FutureSatisfy(&error));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  EXPECT_CALL(*scheduler2, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler2, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    schedulerLibrary2.send(call);
  }

  AWAIT_READY(error);
  AWAIT_READY(disconnected);
  AWAIT_READY(subscribed);

  EXPECT_EQ(frameworkId, subscribed->framework_id());

  Future<v1::executor::Event::Message> message;
  EXPECT_CALL(*executor, message(_, _))
    .WillOnce(FutureArg<1>(&message));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::MESSAGE);

    Call::Message* message = call.mutable_message();
    message->mutable_agent_id()->CopyFrom(offer.agent_id());
    message->mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
    message->set_data("hello world");

    schedulerLibrary2.send(call);
  }

  AWAIT_READY(message);
  ASSERT_EQ("hello world", message->data());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// This test checks that a scheduler exit shuts down the executor.
TEST_F(HttpFaultToleranceTest, SchedulerExit)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos schedulerLibrary(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  Future<Nothing> launch;
  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(FutureSatisfy(&launch));

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", DEFAULT_EXECUTOR_ID));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    schedulerLibrary.send(call);
  }

  AWAIT_READY(launch);

  EXPECT_CALL(*scheduler, disconnected(_))
    .Times(AtMost(1));

  Future<Nothing> shutdown;
  EXPECT_CALL(*executor, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::TEARDOWN);

    schedulerLibrary.send(call);
  }

  // Ensure that the executor receives a `Event::Shutdown` after the
  // scheduler exit.
  AWAIT_READY(shutdown);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
