#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/master.hpp"
#include "master/simple_allocator.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;
using mesos::internal::master::SimpleAllocator;

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
using testing::SaveArgPointee;


TEST(ExceptionTest, AbortOnFrameworkError)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  // Create an invalid ExecutorInfo to trigger framework error.
  MesosSchedulerDriver driver(&sched,
                              "",
                              CREATE_EXECUTOR_INFO(DEFAULT_EXECUTOR_ID, ""),
                              master);

  trigger errorCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(0);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, error(&driver, _, _))
    .WillOnce(Trigger(&errorCall));

  driver.start();

  WAIT_UNTIL(errorCall);

  Status status = driver.join();

  ASSERT_EQ(DRIVER_ABORTED, status);

  driver.stop();

  local::shutdown();
}


TEST(ExceptionTest, DeactiveFrameworkOnAbort)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  trigger deactivateMsg;

  EXPECT_MSG(filter, Eq(DeactivateFrameworkMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(Trigger(&deactivateMsg), Return(false)));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall);

  Status status;

  status = driver.abort();
  ASSERT_EQ(OK, status);

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

  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger schedRegisteredCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .WillOnce(Trigger(&schedRegisteredCall));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(schedRegisteredCall);

  Status status;

  status = driver.abort();
  ASSERT_EQ(OK, status);

  status = driver.reviveOffers();
  ASSERT_EQ(DRIVER_ABORTED, status);

  driver.stop();
  local::shutdown();
}


TEST(ExceptionTest, DisallowSchedulerCallbacksOnAbort)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;

  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  EXPECT_CALL(sched, registered(&driver, _))
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

  EXPECT_CALL(sched, error(&driver, _, _))
      .Times(0);

  process::Message message;
  trigger rescindMsg, unregisterMsg;

  EXPECT_MSG(filter, Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgPointee<0>(&message),Return(false)));

  EXPECT_MSG(filter, Eq(RescindResourceOfferMessage().GetTypeName()), _, _)
    .WillOnce(Trigger(&rescindMsg));

  EXPECT_MSG(filter, Eq(UnregisterFrameworkMessage().GetTypeName()), _, _)
      .WillOnce(Trigger(&unregisterMsg));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);
  EXPECT_NE(0, offers.size());

  Status status;

  status = driver.abort();
  ASSERT_EQ(OK, status);

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
