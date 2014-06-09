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

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <process/metrics/metrics.hpp>

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::AllocatorProcess;

using process::metrics::internal::MetricsProcess;

using process::Clock;
using process::Future;
using process::PID;

using std::string;

using testing::_;
using testing::Eq;
using testing::Return;

// This test case covers tests related to framework API rate limiting
// which includes metrics exporting for API call rates.
class RateLimitingTest : public MesosTest {};


// Verify that message counters for a framework are added when a
// framework registers and removed when it terminates.
TEST_F(RateLimitingTest, FrameworkMessageCounterMetrics)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Message counters not present before the framework registers.
  {
    // TODO(xujyan): It would be nice to refactor out the common
    // metrics snapshot querying logic into a helper method/MACRO.
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        0u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_received"));

    EXPECT_EQ(
        0u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_processed"));
  }

  MockScheduler sched;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver = new MesosSchedulerDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  ASSERT_EQ(DRIVER_RUNNING, driver->start());

  AWAIT_READY(registered);

  // Message counters added after the framework is registered.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_received"));

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_processed"));
  }

  Future<Nothing> frameworkRemoved =
    FUTURE_DISPATCH(_, &AllocatorProcess::frameworkRemoved);

  driver->stop();
  driver->join();
  delete driver;

  AWAIT_READY(frameworkRemoved);

  // Message counter removed after the framework is unregistered.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        0u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_received"));

    EXPECT_EQ(
        0u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_processed"));
  }

  Shutdown();
}


// Verify that framework message counters work with frameworks of
// different principals.
TEST_F(RateLimitingTest, FrameworkMessageCountersMultipleFrameworks)
{
  // 1. Register two frameworks.
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo1; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_principal("framework1");

  MockScheduler sched1;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver1 =
    new MesosSchedulerDriver(&sched1, frameworkInfo1, master.get());

  {
    Future<Nothing> registered;
    EXPECT_CALL(sched1, registered(driver1, _, _))
      .WillOnce(FutureSatisfy(&registered));

    ASSERT_EQ(DRIVER_RUNNING, driver1->start());

    AWAIT_READY(registered);
  }

  FrameworkInfo frameworkInfo2; // Bug in gcc 4.1.*, must assign on next line.
  frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_principal("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  {
    Future<Nothing> registered;
    EXPECT_CALL(sched2, registered(&driver2, _, _))
      .WillOnce(FutureSatisfy(&registered));

    ASSERT_EQ(DRIVER_RUNNING, driver2.start());

    AWAIT_READY(registered);
  }

  // 2. Verify that both frameworks have message counters added.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework1/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework1/messages_processed"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_processed"));
  }

  // 3. Remove a framework.
  Future<Nothing> frameworkRemoved =
    FUTURE_DISPATCH(_, &AllocatorProcess::frameworkRemoved);

  driver1->stop();
  driver1->join();
  delete driver1;

  AWAIT_READY(frameworkRemoved);

  // 4. Its message counters are deleted while the other framework's
  //    counters stay.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        0u, metrics.values.count("frameworks/framework1/messages_received"));
    EXPECT_EQ(
        0u, metrics.values.count("frameworks/framework1/messages_processed"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_processed"));
  }

  driver2.stop();
  driver2.join();

  Shutdown();
}


// Verify that if multiple frameworks use the same principal, they
// share the same counters and removing one framework doesn't remove
// the counters.
TEST_F(RateLimitingTest, FrameworkMessageCountersSamePrincipalFrameworks)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched1;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver1 = new MesosSchedulerDriver(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  {
    Future<Nothing> registered;
    EXPECT_CALL(sched1, registered(driver1, _, _))
      .WillOnce(FutureSatisfy(&registered));

    ASSERT_EQ(DRIVER_RUNNING, driver1->start());

    AWAIT_READY(registered);
  }

  // 'sched2' uses the same principal "test-principal".
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  {
    Future<Nothing> registered;
    EXPECT_CALL(sched2, registered(&driver2, _, _))
      .WillOnce(FutureSatisfy(&registered));

    ASSERT_EQ(DRIVER_RUNNING, driver2.start());

    AWAIT_READY(registered);
  }

  // Message counters added after both frameworks are registered.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_processed"));
  }

  Future<Nothing> frameworkRemoved =
    FUTURE_DISPATCH(_, &AllocatorProcess::frameworkRemoved);

  driver1->stop();
  driver1->join();
  delete driver1;

  AWAIT_READY(frameworkRemoved);

  // Message counters are not removed after the first framework is
  // unregistered.
  {
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/"+ DEFAULT_CREDENTIAL.principal() +
                                 "/messages_processed"));
  }

  driver2.stop();
  driver2.join();

  Shutdown();
}


// Verify that when a scheduler fails over, the new scheduler
// instance continues to use the same counters.
TEST_F(RateLimitingTest, SchedulerMessageCounterFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // 1. Launch the first (i.e., failing) scheduler and verify its
  // counters.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  {
    // Grab this message so we can resend it.
    Future<RegisterFrameworkMessage> registerFrameworkMessage = FUTURE_PROTOBUF(
        RegisterFrameworkMessage(), _, master.get());

    // Grab this message to get the scheduler's pid.
    Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
        Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

    driver1.start();

    AWAIT_READY(registerFrameworkMessage);
    AWAIT_READY(frameworkRegisteredMessage);
    AWAIT_READY(frameworkId);

    const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

    // Send a duplicate RegisterFrameworkMessage. Master replies
    // with a duplicate FrameworkRegisteredMessage.
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), registerFrameworkMessage.get());

    // Now one message has been received and processed by Master in
    // addition to the RegisterFrameworkMessage.
    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Settle to make sure message_processed counters are updated.
    Clock::pause();
    Clock::settle();
    Clock::resume();

    // Verify the message counters.
    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    // One message received and processed after the framework is
    // registered.
    const string& messages_received =
      "frameworks/"+ DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(1, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/"+ DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // 2. Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and verify that
  // its counters are not reset to zero.

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  // Scheduler driver ignores duplicate FrameworkRegisteredMessages.
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .Times(1);

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  {
    // Grab this message to get the scheduler's pid and to make sure we
    // wait until the framework is registered.
    Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
        Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

    // Grab this message so we can resend it.
    Future<ReregisterFrameworkMessage> reregisterFrameworkMessage =
      FUTURE_PROTOBUF(ReregisterFrameworkMessage(), _, master.get());

    driver2.start();

    AWAIT_READY(reregisterFrameworkMessage);
    AWAIT_READY(sched1Error);
    AWAIT_READY(frameworkRegisteredMessage);

    const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    // Sending a duplicate ReregisterFrameworkMessage to test the
    // message counters with the new scheduler instance.
    process::post(schedulerPid, master.get(), reregisterFrameworkMessage.get());

    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Settle to make sure message_processed counters are updated.
    Clock::pause();
    Clock::settle();
    Clock::resume();

    Future<process::http::Response> response =
      process::http::get(MetricsProcess::instance()->self(), "snapshot");
    AWAIT_READY(response);

    EXPECT_SOME_EQ(
        "application/json",
        response.get().headers.get("Content-Type"));

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object metrics = parse.get();

    // Another message after sched2 is reregistered plus the one from
    // the sched1.
    const string& messages_received =
      "frameworks/"+ DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/"+ DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(2, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  Shutdown();
}


// Verify that a framework is being correctly throttled at the
// configured rate.
TEST_F(RateLimitingTest, ThrottleFramework)
{
  // Throttle at 1 QPS.
  RateLimits limits;
  RateLimit* limit = limits.mutable_limits()->Add();
  limit->set_principal(DEFAULT_CREDENTIAL.principal());
  limit->set_qps(1);
  master::Flags flags = CreateMasterFlags();

  flags.rate_limits = JSON::Protobuf(limits);

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  // Grab this message so we can resend it.
  Future<RegisterFrameworkMessage> registerFrameworkMessage = FUTURE_PROTOBUF(
      RegisterFrameworkMessage(), _, master.get());

  // Grab this message to get the scheduler's pid and to make sure we
  // wait until the framework is registered.
  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(registerFrameworkMessage);
  AWAIT_READY(frameworkRegisteredMessage);

  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  Clock::pause();

  // Keep sending duplicate RegisterFrameworkMessages. Master sends
  // FrameworkRegisteredMessage back after processing each of them.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), registerFrameworkMessage.get());
    Clock::settle();

    // The first message is not throttled because it's at the head of
    // the queue.
    AWAIT_READY(duplicateFrameworkRegisteredMessage);
  }

  {
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), registerFrameworkMessage.get());
    Clock::settle();

    // The 2nd message is throttled for a second.
    Clock::advance(Milliseconds(500));
    Clock::settle();

    EXPECT_TRUE(duplicateFrameworkRegisteredMessage.isPending());

    Clock::advance(Milliseconds(501));
    Clock::settle();

    AWAIT_READY(duplicateFrameworkRegisteredMessage);
  }

  Clock::resume();

  EXPECT_EQ(DRIVER_STOPPED, driver.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver.join());

  Shutdown();
}
