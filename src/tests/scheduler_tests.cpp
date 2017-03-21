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
#include <queue>
#include <vector>

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
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/queue.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/lambda.hpp>
#include <stout/try.hpp>

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/detector/standalone.hpp"

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Queue;

using process::http::OK;

using std::cout;
using std::endl;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Return;
using testing::WithParamInterface;

namespace process {

// We need to reinitialize libprocess in order to test against different
// configurations, such as when libprocess is initialized with SSL enabled.
void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readonlyAuthenticationRealm,
    const Option<string>& readwriteAuthenticationRealm);

} // namespace process {

namespace mesos {
namespace internal {
namespace tests {


class SchedulerTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// The scheduler library tests are parameterized by the content type
// of the HTTP request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    SchedulerTest,
    ::testing::Values(ContentType::PROTOBUF, ContentType::JSON));


// This test verifies that a scheduler can subscribe with the master.
TEST_P(SchedulerTest, Subscribe)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  ASSERT_EQ(master::DEFAULT_HEARTBEAT_INTERVAL.secs(),
            subscribed->heartbeat_interval_seconds());
  ASSERT_EQ(evolve(master.get()->getMasterInfo()), subscribed->master_info());
}


// This test verifies that a scheduler can subscribe with the master after
// failing over to another instance.
TEST_P(SchedulerTest, SchedulerFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId = subscribed->framework_id();

  auto scheduler2 = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected2;
  EXPECT_CALL(*scheduler2, connected(_))
    .WillOnce(FutureSatisfy(&connected2));

  // Failover to another scheduler instance.
  v1::scheduler::TestMesos mesos2(
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

    mesos2.send(call);
  }

  AWAIT_READY(error);
  AWAIT_READY(disconnected);
  AWAIT_READY(subscribed);

  EXPECT_EQ(frameworkId, subscribed->framework_id());
}


// This test verifies that the scheduler can subscribe after a master failover.
TEST_P_TEMP_DISABLED_ON_WINDOWS(SchedulerTest, MasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler,
      detector);

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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId = subscribed->framework_id();

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  // Failover the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  AWAIT_READY(disconnected);

  Future<Nothing> connected2;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected2));

  detector->appoint(master.get()->pid);

  AWAIT_READY(connected2);

  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);
    subscribe->mutable_framework_info()->mutable_id()->CopyFrom(frameworkId);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  EXPECT_EQ(frameworkId, subscribed->framework_id());
}


// This test verifies that scheduler library also exposes metrics like
// scheduler driver.
TEST_P(SchedulerTest, MetricsEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  Future<process::http::Response> response =
    process::http::get(process::metrics::internal::metrics, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(parse);

  JSON::Object metrics = parse.get();

  // "scheduler/event_queue_messages" metric reports the number of message
  // events in event queue. Message events are invoked when any custom
  // message is generated by the executor.
  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_messages"));

  // "scheduler/event_queue_dispatches" metric reports the number of dispatch
  // events in event queue. Dispatch events are invoked when any function is
  // dispatched as process as a result of any call by scheduler.
  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_dispatches"));
}


TEST_P(SchedulerTest, TaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged));

  Future<Event::Update> statusUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  Future<Nothing> update;
  EXPECT_CALL(containerizer, update(_, _))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())))
    .WillRepeatedly(Return(Future<Nothing>())); // Ignore subsequent calls.

  v1::TaskInfo taskInfo;
  taskInfo.set_name("");
  taskInfo.mutable_task_id()->set_value("1");
  taskInfo.mutable_agent_id()->CopyFrom(
      offers->offers(0).agent_id());
  taskInfo.mutable_resources()->CopyFrom(
      offers->offers(0).resources());
  taskInfo.mutable_executor()->CopyFrom(v1::DEFAULT_EXECUTOR_INFO);

  // TODO(benh): Enable just running a task with a command in the tests:
  //   taskInfo.mutable_command()->set_value("sleep 10");

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offers->offers(0).id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(statusUpdate);

  EXPECT_EQ(v1::TASK_RUNNING, statusUpdate->status().state());
  EXPECT_TRUE(statusUpdate->status().has_executor_id());
  EXPECT_EQ(executorId, devolve(statusUpdate->status().executor_id()));

  AWAIT_READY(update);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// Ensures that a task group can be successfully launched
// on the `DEFAULT` executor.
//
// TODO(bmahler): We currently only test the master-side
// of task group handling, since the rest is unimplemented.
TEST_P(SchedulerTest, TaskGroupRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  Future<RunTaskGroupMessage> runTaskGroupMessage =
    FUTURE_PROTOBUF(RunTaskGroupMessage(), master.get()->pid, slave.get()->pid);

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executor;
  executor.set_type(v1::ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("E");
  executor.mutable_framework_id()->CopyFrom(subscribed->framework_id());
  executor.mutable_resources()->CopyFrom(resources);

  v1::TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_agent_id()->CopyFrom(
      offers->offers(0).agent_id());
  task1.mutable_resources()->CopyFrom(resources);

  v1::TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_agent_id()->CopyFrom(
      offers->offers(0).agent_id());
  task2.mutable_resources()->CopyFrom(resources);

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offers->offers(0).id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  // TODO(bmahler): For now we only ensure that the message is
  // sent to the agent, since the agent-side of task groups is
  // not yet implemented.

  AWAIT_READY(runTaskGroupMessage);

  EXPECT_EQ(devolve(frameworkId), runTaskGroupMessage->framework().id());

  EXPECT_EQ(devolve(executor.executor_id()),
            runTaskGroupMessage->executor().executor_id());

  ASSERT_EQ(2, runTaskGroupMessage->task_group().tasks().size());
  EXPECT_EQ(devolve(task1.task_id()),
            runTaskGroupMessage->task_group().tasks(0).task_id());
  EXPECT_EQ(devolve(task2.task_id()),
            runTaskGroupMessage->task_group().tasks(1).task_id());
}


TEST_P(SchedulerTest, ReconcileTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged));

  Future<Event::Update> update1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1));

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

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update1);

  EXPECT_EQ(v1::TASK_RUNNING, update1->status().state());

  Future<Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update2));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::RECONCILE);

    Call::Reconcile::Task* task = call.mutable_reconcile()->add_tasks();
    task->mutable_task_id()->CopyFrom(taskInfo.task_id());

    mesos.send(call);
  }

  AWAIT_READY(update2);

  EXPECT_FALSE(update2->status().has_uuid());
  EXPECT_EQ(v1::TASK_RUNNING, update2->status().state());
  EXPECT_EQ(v1::TaskStatus::REASON_RECONCILIATION,
            update2->status().reason());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


TEST_P(SchedulerTest, KillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged))
    .WillRepeatedly(Return());

  Future<Event::Update> update1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1));

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

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update1);

  EXPECT_EQ(v1::TASK_RUNNING, update1->status().state());

  {
    // Acknowledge TASK_RUNNING update.
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  Future<Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update2));

  EXPECT_CALL(*executor, kill(_, _))
    .WillOnce(v1::executor::SendUpdateFromTaskID(
        frameworkId, evolve(executorId), v1::TASK_KILLED));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo.task_id());
    kill->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

  AWAIT_READY(update2);

  EXPECT_EQ(v1::TASK_KILLED, update2->status().state());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


TEST_P(SchedulerTest, ShutdownExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_FINISHED));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged));

  Future<Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

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

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update);

  EXPECT_EQ(v1::TASK_FINISHED, update->status().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(*executor, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  Future<Event::Failure> failure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&failure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SHUTDOWN);

    Call::Shutdown* shutdown = call.mutable_shutdown();
    shutdown->mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
    shutdown->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

  AWAIT_READY(shutdown);
  containerizer.destroy(devolve(frameworkId), executorId);

  // Executor termination results in a 'FAILURE' event.
  AWAIT_READY(failure);
  EXPECT_EQ(executorId, devolve(failure->executor_id()));
}


TEST_P(SchedulerTest, Decline)
{
  master::Flags flags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers1);
  ASSERT_EQ(1, offers1->offers().size());

  const v1::Offer& offer = offers1->offers(0);

  Future<Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(offer.id());

    // Set 0s filter to immediately get another offer.
    v1::Filters filters;
    filters.set_refuse_seconds(0);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  // Make sure the dispatch event for `recoverResources` has been enqueued.
  AWAIT_READY(recoverResources);

  Clock::pause();
  Clock::advance(flags.allocation_interval);
  Clock::resume();

  // If the resources were properly declined, the scheduler should
  // get another offer with same amount of resources.
  AWAIT_READY(offers2);
  ASSERT_EQ(1, offers2->offers().size());
  ASSERT_EQ(offer.resources(), offers2->offers(0).resources());
}


TEST_P(SchedulerTest, Revive)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers1);
  EXPECT_NE(0, offers1->offers().size());

  const v1::Offer& offer = offers1->offers(0);

  Future<Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(offer.id());

    // Set 1hr filter to not immediately get another offer.
    v1::Filters filters;
    filters.set_refuse_seconds(Hours(1).secs());
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  // No offers should be sent within 30 mins because we set a filter
  // for 1 hr.
  Clock::pause();
  Clock::advance(Minutes(30));
  Clock::settle();

  ASSERT_TRUE(offers2.isPending());

  // On revival the filters should be cleared and the scheduler should
  // get another offer with same amount of resources.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::REVIVE);

    mesos.send(call);
  }

  AWAIT_READY(offers2);
  EXPECT_NE(0, offers2->offers().size());
  ASSERT_EQ(offer.resources(), offers2->offers(0).resources());
}


TEST_P(SchedulerTest, Suppress)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers1);
  EXPECT_NE(0, offers1->offers().size());

  const v1::Offer& offer = offers1->offers(0);

  Future<Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(offer.id());

    // Set 1hr filter to not immediately get another offer.
    v1::Filters filters;
    filters.set_refuse_seconds(Hours(1).secs());
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  Future<Nothing> suppressOffers =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::suppressOffers);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SUPPRESS);

    mesos.send(call);
  }

  AWAIT_READY(suppressOffers);

  // Wait for allocator to finish executing 'suppressOffers()'.
  Clock::pause();
  Clock::settle();

  // No offers should be sent within 100 mins because the framework
  // suppressed offers.
  Clock::advance(Minutes(100));
  Clock::settle();

  ASSERT_TRUE(offers2.isPending());

  // On reviving offers the scheduler should get another offer with same amount
  // of resources.
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::REVIVE);

    mesos.send(call);
  }

  AWAIT_READY(offers2);

  EXPECT_NE(0, offers2->offers().size());
  ASSERT_EQ(offer.resources(), offers2->offers(0).resources());
}


TEST_P(SchedulerTest, Message)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

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

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update);

  EXPECT_EQ(v1::TASK_RUNNING, update->status().state());

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

    mesos.send(call);
  }

  AWAIT_READY(message);
  ASSERT_EQ("hello world", message->data());

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


TEST_P(SchedulerTest, Request)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  Future<Nothing> requestResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::requestResources);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::REQUEST);

    // Create a dummy request.
    Call::Request* request = call.mutable_request();
    request->add_requests();

    mesos.send(call);
  }

  AWAIT_READY(requestResources);
}


// This test verifies that the scheduler is able to force a reconnection with
// the master.
TEST_P(SchedulerTest, SchedulerReconnect)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto detector = std::make_shared<StandaloneMasterDetector>(master.get()->pid);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  ContentType contentType = GetParam();

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler,
      detector);

  AWAIT_READY(connected);

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  // Force a reconnection with the master. This should result in a
  // `disconnected` callback followed by a `connected` callback.
  mesos.reconnect();

  AWAIT_READY(disconnected);

  // The scheduler should be able to immediately reconnect with the master.
  AWAIT_READY(connected);

  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious master failure event at the scheduler.
  detector->appoint(None());

  AWAIT_READY(disconnected);

  EXPECT_CALL(*scheduler, disconnected(_))
    .Times(0);

  EXPECT_CALL(*scheduler, connected(_))
    .Times(0);

  mesos.reconnect();

  // Flush any possible remaining events. The mocked scheduler will fail if the
  // reconnection attempt resulted in any additional callbacks after the
  // scheduler has disconnected.
  Clock::pause();
  Clock::settle();
}


// TODO(benh): Write test for sending Call::Acknowledgement through
// master to slave when Event::Update was generated locally.


class SchedulerReconcileTasks_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<size_t> {};


// The scheduler reconcile benchmark tests are parameterized by the number of
// tasks that need to be reconciled.
INSTANTIATE_TEST_CASE_P(
    Tasks,
    SchedulerReconcileTasks_BENCHMARK_Test,
    ::testing::Values(1000U, 10000U, 50000U, 100000U));


// This benchmark simulates a large reconcile request containing tasks unknown
// to the master using the scheduler library/driver. It then measures the time
// required for processing the received `TASK_LOST` status updates.
TEST_P(SchedulerReconcileTasks_BENCHMARK_Test, SchedulerLibrary)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  const size_t tasks = GetParam();

  EXPECT_CALL(*scheduler, update(_, _))
    .Times(tasks);

  Call call;
  call.mutable_framework_id()->CopyFrom(frameworkId);
  call.set_type(Call::RECONCILE);

  for (size_t i = 0; i < tasks; ++i) {
    Call::Reconcile::Task* task = call.mutable_reconcile()->add_tasks();
    task->mutable_task_id()->set_value("task " + stringify(i));
  }

  Stopwatch watch;
  watch.start();

  mesos.send(call);

  Clock::pause();
  Clock::settle();

  cout << "Reconciling " << tasks << " tasks took " << watch.elapsed()
       << " using the scheduler library" << endl;
}


TEST_P(SchedulerReconcileTasks_BENCHMARK_Test, SchedulerDriver)
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

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver.start();

  AWAIT_READY(frameworkId);

  const size_t tasks = GetParam();

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(tasks);

  vector<TaskStatus> statuses;

  for (size_t i = 0; i < tasks; ++i) {
    TaskStatus status;
    status.mutable_task_id()->set_value("task " + stringify(i));

    statuses.push_back(status);
  }

  Stopwatch watch;
  watch.start();

  driver.reconcileTasks(statuses);

  Clock::pause();
  Clock::settle();

  cout << "Reconciling " << tasks << " tasks took " << watch.elapsed()
       << " using the scheduler driver" << endl;

  driver.stop();
  driver.join();
}


// A fixture class for scheduler tests that can be run with SSL either enabled
// or disabled.
class SchedulerSSLTest
  : public MesosTest,
    public WithParamInterface<std::tr1::tuple<ContentType, string>>
{
// These test setup/teardown methods are only needed when compiled with SSL.
#ifdef USE_SSL_SOCKET
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    if (std::tr1::get<1>(GetParam()) == "https") {
      generate_keys_and_certs();
      set_environment_variables({
          {"LIBPROCESS_SSL_ENABLED", "true"},
          {"LIBPROCESS_SSL_KEY_FILE", key_path()},
          {"LIBPROCESS_SSL_CERT_FILE", certificate_path()},
          {"LIBPROCESS_SSL_CA_FILE", certificate_path().string()},
          {"LIBPROCESS_SSL_REQUIRE_CERT", "true"}});
      process::reinitialize(
          None(),
          READONLY_HTTP_AUTHENTICATION_REALM,
          READWRITE_HTTP_AUTHENTICATION_REALM);
    } else {
      set_environment_variables({});
      process::reinitialize(
          None(),
          READONLY_HTTP_AUTHENTICATION_REALM,
          READWRITE_HTTP_AUTHENTICATION_REALM);
    }
  }

public:
  static void TearDownTestCase()
  {
    // The teardown code of `MesosTest` calls `set_environment_variables({})`,
    // so we invoke it first.
    MesosTest::TearDownTestCase();

    process::reinitialize(
        None(),
        READONLY_HTTP_AUTHENTICATION_REALM,
        READWRITE_HTTP_AUTHENTICATION_REALM);
  }
#endif // USE_SSL_SOCKET
};


// NOTE: `#ifdef`'ing out the argument `string("https")` argument causes a
// build break on Windows, because the preprocessor is not required to to
// process the text it expands.
#ifdef USE_SSL_SOCKET
INSTANTIATE_TEST_CASE_P(
    ContentTypeAndSSLConfig,
    SchedulerSSLTest,
    ::testing::Combine(
        ::testing::Values(ContentType::PROTOBUF, ContentType::JSON),
        ::testing::Values(
            string("https"),
            string("http"))));
#else
INSTANTIATE_TEST_CASE_P(
    ContentTypeAndSSLConfig,
    SchedulerSSLTest,
    ::testing::Combine(
        ::testing::Values(ContentType::PROTOBUF, ContentType::JSON),
        ::testing::Values(
            string("http"))));
#endif // USE_SSL_SOCKET


// Tests that a scheduler can subscribe, run a task, and then tear itself down.
TEST_P(SchedulerSSLTest, RunTaskAndTeardown)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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

  ContentType contentType = std::tr1::get<0>(GetParam());

  v1::scheduler::TestMesos mesos(master.get()->pid, contentType, scheduler);

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

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

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

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update);

  EXPECT_EQ(v1::TASK_RUNNING, update->status().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(*executor, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::TEARDOWN);

    mesos.send(call);
  }

  AWAIT_READY(shutdown);
  AWAIT_READY(disconnected);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
