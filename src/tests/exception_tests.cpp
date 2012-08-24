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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AnyOf;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;


TEST(ExceptionTest, DeactiveFrameworkOnAbort)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  trigger deactivateMsg;

  EXPECT_MESSAGE(filter, Eq(DeactivateFrameworkMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&deactivateMsg), Return(false)));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall);

  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  WAIT_UNTIL(deactivateMsg);

  driver.stop();
  local::shutdown();

  process::filter(NULL);
}


TEST(ExceptionTest, DisallowSchedulerActionsOnAbort)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall);

  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  ASSERT_EQ(DRIVER_ABORTED, driver.reviveOffers());

  driver.stop();
  local::shutdown();
}


TEST(ExceptionTest, DisallowSchedulerCallbacksOnAbort)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  trigger resourceOffersCall;
  vector<Offer> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

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

  process::Message message;
  trigger rescindMsg, unregisterMsg;

  EXPECT_MESSAGE(filter, Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  EXPECT_MESSAGE(filter, Eq(RescindResourceOfferMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&rescindMsg), Return(false)));

  EXPECT_MESSAGE(filter, Eq(UnregisterFrameworkMessage().GetTypeName()), _, _)
      .WillOnce(DoAll(Trigger(&unregisterMsg), Return(false)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);
  EXPECT_NE(0u, offers.size());


  ASSERT_EQ(DRIVER_ABORTED, driver.abort());

  // Simulate a message from master to the scheduler.
  RescindResourceOfferMessage rescindMessage;
  rescindMessage.mutable_offer_id()->MergeFrom(offers[0].id());

  process::post(message.to, rescindMessage);

  WAIT_UNTIL(rescindMsg);

  driver.stop();

  WAIT_UNTIL(unregisterMsg); //Ensures reception of RescindResourceOfferMessage.

  local::shutdown();

  process::filter(NULL);
}
