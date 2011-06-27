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

using mesos::internal::slave::ProcessBasedIsolationModule;
using mesos::internal::slave::Slave;
using mesos::internal::slave::STATUS_UPDATE_RETRY_INTERVAL_SECONDS;

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


TEST(MasterTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");
  
  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  trigger offerRescindedCall, slaveLostCall;

  EXPECT_CALL(sched, offerRescinded(&driver, offerId))
    .WillOnce(Trigger(&offerRescindedCall));

  EXPECT_CALL(sched, slaveLost(&driver, offers[0].slave_id()))
    .WillOnce(Trigger(&slaveLostCall));

  process::post(slave, process::TERMINATE);

  WAIT_UNTIL(offerRescindedCall);
  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}


TEST(MasterTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  process::Clock::pause();

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  trigger slaveLostCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(Trigger(&slaveLostCall));

  EXPECT_MSG(filter, Eq("PONG"), _, _)
    .WillRepeatedly(Return(true));

  driver.start();

  double secs = master::SLAVE_PONG_TIMEOUT * master::MAX_SLAVE_TIMEOUTS;

  process::Clock::advance(secs);

  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  local::shutdown();

  process::filter(NULL);

  process::Clock::resume();
}


TEST(MasterTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;

  trigger sched1RegisteredCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&frameworkId), Trigger(&sched1RegisteredCall)));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1RegisteredCall);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger sched2RegisteredCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  process::Clock::pause();

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  Master m;
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;
  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall, statusUpdateMsg;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  EXPECT_MSG(filter, Eq(StatusUpdateMessage().GetTypeName()), _,
             Not(AnyOf(Eq(master), Eq(slave))))
    .WillOnce(DoAll(Trigger(&statusUpdateMsg), Return(true)))
    .RetiresOnSaturation();

  driver1.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateMsg);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers, at which point advance time enough for the reliable
  // timeout to kick in and another status update message is sent.

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger registeredCall, statusUpdateCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(Trigger(&statusUpdateCall));

  driver2.start();

  WAIT_UNTIL(registeredCall);

  process::Clock::advance(STATUS_UPDATE_RETRY_INTERVAL_SECONDS);

  WAIT_UNTIL(statusUpdateCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);

  process::filter(NULL);

  process::Clock::resume();
}


TEST(MasterTest, SchedulerFailoverFrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;
  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger sched1ResourceOfferCall, sched1StatusUpdateCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&sched1StatusUpdateCall)));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, ElementsAre(_)))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(sched1StatusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger sched2RegisteredCall, sched2FrameworkMessageCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, frameworkMessage(&driver2, _, _, _))
    .WillOnce(Trigger(&sched2FrameworkMessageCall));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  execDriver->sendFrameworkMessage("");

  WAIT_UNTIL(sched2FrameworkMessageCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  process::post(slave, process::TERMINATE);
  process::wait(slave);

  process::post(master, process::TERMINATE);
  process::wait(master);
}
