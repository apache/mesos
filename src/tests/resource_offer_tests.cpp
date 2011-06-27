#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Return;
using testing::SaveArg;


TEST(MasterTest, ResourceOfferWithMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(10, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<2>(&offers), Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());
  EXPECT_GE(10, offers.size());

  Resources resources(offers[0].resources());
  EXPECT_EQ(2, resources.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(1024, resources.getScalar("mem", Resource::Scalar()).value());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  driver1.replyToOffer(offerId, vector<TaskDescription>());

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterBadResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, ElementsAre(_)))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Resource::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  Resource* mem = task.add_resources();
  mem->set_name("mem");
  mem->set_type(Resource::SCALAR);
  mem->mutable_scalar()->set_value(1 * Gigabyte);

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  trigger sched1ErrorCall;

  EXPECT_CALL(sched1,
              error(&driver1, _, "Invalid resources for task"))
    .WillOnce(Trigger(&sched1ErrorCall));

  EXPECT_CALL(sched1, offerRescinded(&driver1, offerId))
    .Times(AtMost(1));

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(sched1ErrorCall);

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}
