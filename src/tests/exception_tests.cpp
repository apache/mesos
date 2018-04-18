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

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;
using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class ExceptionTest : public MesosTest {};


TEST_F(ExceptionTest, DeactivateFrameworkOnAbort)
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

  driver.start();

  AWAIT_READY(registered);

  Future<DeactivateFrameworkMessage> deactivateFrameworkMessage =
    FUTURE_PROTOBUF(DeactivateFrameworkMessage(), _, _);

  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  AWAIT_READY(deactivateFrameworkMessage);

  driver.stop();
}


TEST_F(ExceptionTest, DisallowSchedulerActionsOnAbort)
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

  driver.start();

  AWAIT_READY(registered);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  ASSERT_EQ(DRIVER_ABORTED, driver.reviveOffers());

  driver.stop();
}


TEST_F(ExceptionTest, DisallowSchedulerCallbacksOnAbort)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // None of these callbacks should be invoked.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  EXPECT_CALL(sched, frameworkMessage(&driver, _, _, _))
    .Times(0);

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(0);

  EXPECT_CALL(sched, error(&driver, _))
    .Times(0);

  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  Future<RescindResourceOfferMessage> rescindMsg =
    FUTURE_PROTOBUF(RescindResourceOfferMessage(), _, _);

  // Simulate a message from master to the scheduler.
  RescindResourceOfferMessage rescindMessage;
  rescindMessage.mutable_offer_id()->MergeFrom(offers.get()[0].id());

  process::post(message->to, rescindMessage);

  AWAIT_READY(rescindMsg);

  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  driver.stop();

  // Ensures reception of RescindResourceOfferMessage.
  AWAIT_READY(teardownCall);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
