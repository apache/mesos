#include <boost/lexical_cast.hpp>

#include <gmock/gmock.h>

#include <mesos_exec.hpp>
#include <mesos_sched.hpp>

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/isolation_module.hpp"
#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using namespace mesos;
using namespace mesos::internal;

using boost::lexical_cast;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Framework;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using std::string;
using std::vector;

using testing::_;
using testing::A;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::ElementsAre;
using testing::Ne;
using testing::Return;
using testing::SaveArg;
using testing::Sequence;
using testing::StrEq;


class MockScheduler : public Scheduler
{
public:
  MOCK_METHOD1(getFrameworkName, std::string(SchedulerDriver*));
  MOCK_METHOD1(getExecutorInfo, ExecutorInfo(SchedulerDriver*));
  MOCK_METHOD2(registered, void(SchedulerDriver*, FrameworkID));
  MOCK_METHOD3(resourceOffer, void(SchedulerDriver*, OfferID,
                                   const std::vector<SlaveOffer>&));
  MOCK_METHOD2(offerRescinded, void(SchedulerDriver*, OfferID));
  MOCK_METHOD2(statusUpdate, void(SchedulerDriver*, const TaskStatus&));
  MOCK_METHOD2(frameworkMessage, void(SchedulerDriver*,
                                      const FrameworkMessage&));
  MOCK_METHOD2(slaveLost, void(SchedulerDriver*, SlaveID));
  MOCK_METHOD3(error, void(SchedulerDriver*, int, const std::string&));
};


class MockExecutor : public Executor
{
public:
  MOCK_METHOD2(init, void(ExecutorDriver*, const ExecutorArgs&));
  MOCK_METHOD2(launchTask, void(ExecutorDriver*, const TaskDescription&));
  MOCK_METHOD2(killTask, void(ExecutorDriver*, TaskID));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const FrameworkMessage&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD3(error, void(ExecutorDriver*, int, const std::string&));
};


class MockFilter : public MessageFilter
{
 public:
  MOCK_METHOD1(filter, bool(struct msg *));
};


struct trigger
{
  trigger() : value(false) {}
  bool value;
};


MATCHER_P3(MsgMatcher, id, from, to, "")
{
  return (testing::Matcher<MSGID>(id).Matches(arg->id) &&
          testing::Matcher<PID>(from).Matches(arg->from) &&
          testing::Matcher<PID>(to).Matches(arg->to));
}


ACTION_P(Trigger, trigger) { trigger->value = true; }


#define EXPECT_MSG(filter, id, from, to)                \
  EXPECT_CALL(filter, filter(MsgMatcher(id, from, to)))


#define WAIT_UNTIL(trigger)                                             \
  do {                                                                  \
    int sleeps = 0;                                                     \
    do {                                                                \
      __sync_synchronize();                                             \
      if (trigger.value)                                                \
        break;                                                          \
      usleep(10);                                                       \
      if (sleeps++ >= 100000) {                                         \
        ADD_FAILURE();                                                  \
        break;                                                          \
      }                                                                 \
    } while (true);                                                     \
  } while (false)


class LocalIsolationModule : public IsolationModule
{
public:
  Executor *executor;
  MesosExecutorDriver *driver;
  string pid;

  LocalIsolationModule(Executor *_executor)
    : executor(_executor), driver(NULL) {}

  virtual ~LocalIsolationModule() {}

  virtual void initialize(Slave *slave) {
    pid = slave->self();
  }

  virtual void startExecutor(Framework *framework) {
    // TODO(benh): Cleanup the way we launch local drivers!
    setenv("MESOS_LOCAL", "1", 1);
    setenv("MESOS_SLAVE_PID", pid.c_str(), 1);
    setenv("MESOS_FRAMEWORK_ID", framework->id.c_str(), 1);

    driver = new MesosExecutorDriver(executor);
    driver->start();
  }

  virtual void killExecutor(Framework* framework) {
    driver->stop();
    driver->join();
    delete driver;

    // TODO(benh): Cleanup the way we launch local drivers!
    unsetenv("MESOS_LOCAL");
    unsetenv("MESOS_SLAVE_PID");
    unsetenv("MESOS_FRAMEWORK_ID");
  }
};


TEST(MasterTest, ResourceOfferForMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<2>(&offers), Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_GE(10, offers.size());
  EXPECT_EQ("2", offers[0].params["cpus"]);
  EXPECT_EQ("1024", offers[0].params["mem"]);

  driver.stop();
  driver.join();

  local::shutdown();
}


class ReplyToOfferErrorTest : public testing::Test
{
protected:
  ReplyToOfferErrorTest() : driver(NULL)
  {
    PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
    driver = new MesosSchedulerDriver(&sched, master);
  }

  virtual ~ReplyToOfferErrorTest()
  {
    delete driver;
  }

  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    trigger resourceOfferCall;

    EXPECT_CALL(sched, getFrameworkName(driver))
      .WillOnce(Return(""));

    EXPECT_CALL(sched, getExecutorInfo(driver))
      .WillOnce(Return(ExecutorInfo("noexecutor", "")));

    EXPECT_CALL(sched, registered(driver, _))
      .Times(1);

    EXPECT_CALL(sched, resourceOffer(driver, _, ElementsAre(_)))
      .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                      Trigger(&resourceOfferCall)));

    driver->start();

    WAIT_UNTIL(resourceOfferCall);

    ASSERT_GE(1, offers.size());
    EXPECT_EQ("3", offers[0].params["cpus"]);
    EXPECT_EQ("3072", offers[0].params["mem"]);
  }

  virtual void TearDown()
  {
    ASSERT_NE("", message);

    trigger errorCall;

    EXPECT_CALL(sched, error(driver, _, message))
      .WillOnce(Trigger(&errorCall));

    EXPECT_CALL(sched, offerRescinded(driver, offerId))
      .Times(AtMost(1));

    driver->replyToOffer(offerId, tasks, map<string, string>());

    WAIT_UNTIL(errorCall);

    driver->stop();
    driver->join();

    local::shutdown();
  }

  MockScheduler sched;
  MesosSchedulerDriver *driver;

  OfferID offerId;
  vector<SlaveOffer> offers;

  map<string, string> params;
  vector<TaskDescription> tasks;
  string message;
};


TEST_F(ReplyToOfferErrorTest, DuplicateTaskIdsInResponse)
{
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));
  tasks.push_back(TaskDescription(2, offers[0].slaveId, "", params, bytes()));
  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));

  message = "Duplicate task ID: 1";
}


TEST_F(ReplyToOfferErrorTest, TooMuchMemoryInTask)
{
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(4 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));

  message = "Too many resources accepted";
}


TEST_F(ReplyToOfferErrorTest, TooMuchCpuInTask)
{
  params["cpus"] = "4";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));
  message = "Too many resources accepted";
}


TEST_F(ReplyToOfferErrorTest, TooLittleCpuInTask)
{
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));
  message = "Invalid task size: <0 CPUs, 1024 MEM>";
}


TEST_F(ReplyToOfferErrorTest, TooLittleMemoryInTask)
{
  params["cpus"] = "1";
  params["mem"] = "1";

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));

  message = "Invalid task size: <1 CPUs, 1 MEM>";
}


TEST_F(ReplyToOfferErrorTest, TooMuchMemoryAcrossTasks)
{
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(2 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));
  tasks.push_back(TaskDescription(2, offers[0].slaveId, "", params, bytes()));

  message = "Too many resources accepted";
}


TEST_F(ReplyToOfferErrorTest, TooMuchCpuAcrossTasks)
{
  params["cpus"] = "2";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);

  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));
  tasks.push_back(TaskDescription(2, offers[0].slaveId, "", params, bytes()));

  message = "Too many resources accepted";
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), Trigger(&sched1ResourceOfferCall)));

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  driver1.replyToOffer(offerId, vector<TaskDescription>(), map<string, string>());

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall));

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

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, ElementsAre(_)))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&sched1ResourceOfferCall)));

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  ASSERT_GE(1, offers.size());

  map<string, string> params;
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);

  vector<TaskDescription> tasks;
  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", params, bytes()));

  trigger errorCall;

  EXPECT_CALL(sched1, error(&driver1, _, "Invalid task size: <0 CPUs, 1024 MEM>"))
    .WillOnce(Trigger(&errorCall));

  EXPECT_CALL(sched1, offerRescinded(&driver1, offerId))
    .Times(AtMost(1));

  driver1.replyToOffer(offerId, tasks, map<string, string>());

  WAIT_UNTIL(errorCall);

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall));

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;
  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_GE(1, offers.size());

  trigger offerRescindedCall, slaveLostCall;

  EXPECT_CALL(sched, offerRescinded(&driver, offerId))
    .WillOnce(Trigger(&offerRescindedCall));

  EXPECT_CALL(sched, slaveLost(&driver, offers[0].slaveId))
    .WillOnce(Trigger(&slaveLostCall));

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());

  WAIT_UNTIL(offerRescindedCall);
  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}


TEST(MasterTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler failingSched;
  MesosSchedulerDriver failingDriver(&failingSched, master);

  FrameworkID frameworkId;

  trigger failingRegisteredCall;

  EXPECT_CALL(failingSched, getFrameworkName(&failingDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(failingSched, getExecutorInfo(&failingDriver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(failingSched, registered(&failingDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&frameworkId), Trigger(&failingRegisteredCall)));

  EXPECT_CALL(failingSched, resourceOffer(&failingDriver, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(failingSched, offerRescinded(&failingDriver, _))
    .Times(AtMost(1));

  EXPECT_CALL(failingSched, error(&failingDriver, _, "Framework failover"))
    .Times(1);

  failingDriver.start();

  WAIT_UNTIL(failingRegisteredCall);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler failoverSched;
  MesosSchedulerDriver failoverDriver(&failoverSched, master, frameworkId);

  trigger failoverRegisteredCall;

  EXPECT_CALL(failoverSched, getFrameworkName(&failoverDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(failoverSched, getExecutorInfo(&failoverDriver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(failoverSched, registered(&failoverDriver, frameworkId))
    .WillOnce(Trigger(&failoverRegisteredCall));

  EXPECT_CALL(failoverSched, resourceOffer(&failoverDriver, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(failoverSched, offerRescinded(&failoverDriver, _))
    .Times(AtMost(1));

  failoverDriver.start();

  WAIT_UNTIL(failoverRegisteredCall);

  failingDriver.stop();
  failoverDriver.stop();

  failingDriver.join();
  failoverDriver.join();

  local::shutdown();
}


TEST(MasterTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  ProcessClock::pause();

  MockFilter filter;
  Process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  trigger slaveLostCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(Trigger(&slaveLostCall));

  EXPECT_MSG(filter, Eq(SH2M_HEARTBEAT), _, _)
    .WillRepeatedly(Return(true));

  driver.start();

  ProcessClock::advance(master::HEARTBEAT_TIMEOUT);

  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  local::shutdown();

  Process::filter(NULL);

  ProcessClock::resume();
}


TEST(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger resourceOfferCall, statusUpdateCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_GE(1, offers.size());

  vector<TaskDescription> tasks;
  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", offers[0].params, ""));

  driver.replyToOffer(offerId, tasks, map<string, string>());

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state);

  driver.stop();
  driver.join();

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}


TEST(MasterTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  ProcessClock::pause();

  MockFilter filter;
  Process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Master m;
  PID master = Process::spawn(&m);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler failingSched;
  MesosSchedulerDriver failingDriver(&failingSched, master);

  FrameworkID frameworkId;
  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(failingSched, getFrameworkName(&failingDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(failingSched, getExecutorInfo(&failingDriver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(failingSched, registered(&failingDriver, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(failingSched, resourceOffer(&failingDriver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(failingSched, error(&failingDriver, _, "Framework failover"))
    .Times(1);

  EXPECT_CALL(failingSched, statusUpdate(&failingDriver, _))
    .Times(0);

  trigger statusUpdateMsg;

  EXPECT_MSG(filter, Eq(S2M_FT_STATUS_UPDATE), _, Ne(master))
    .WillOnce(DoAll(Trigger(&statusUpdateMsg), Return(true)))
    .RetiresOnSaturation();

  failingDriver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_GE(1, offers.size());

  vector<TaskDescription> tasks;
  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", offers[0].params, ""));

  failingDriver.replyToOffer(offerId, tasks, map<string, string>());

  WAIT_UNTIL(statusUpdateMsg);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers, at which point advance time enough for the reliable
  // timeout to kick in and another status update message is sent.

  MockScheduler failoverSched;
  MesosSchedulerDriver failoverDriver(&failoverSched, master, frameworkId);

  trigger registeredCall, statusUpdateCall;

  EXPECT_CALL(failoverSched, getFrameworkName(&failoverDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(failoverSched, getExecutorInfo(&failoverDriver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(failoverSched, registered(&failoverDriver, frameworkId))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(failoverSched, statusUpdate(&failoverDriver, _))
    .WillOnce(Trigger(&statusUpdateCall));

  failoverDriver.start();

  WAIT_UNTIL(registeredCall);

  ProcessClock::advance(RELIABLE_TIMEOUT);

  WAIT_UNTIL(statusUpdateCall);

  failingDriver.stop();
  failoverDriver.stop();

  failingDriver.join();
  failoverDriver.join();

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);

  Process::filter(NULL);

  ProcessClock::resume();
}


TEST(MasterTest, FrameworkMessages)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockExecutor exec;

  ExecutorDriver *execDriver;
  ExecutorArgs args;
  FrameworkMessage execMessage;

  trigger execFrameworkMessageCall;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver), SaveArg<1>(&args)));

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(DoAll(SaveArg<1>(&execMessage),
                    Trigger(&execFrameworkMessageCall)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Master m;
  PID master = Process::spawn(&m);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;
  FrameworkMessage schedMessage;

  trigger resourceOfferCall, statusUpdateCall, schedFrameworkMessageCall;

  EXPECT_CALL(sched, getFrameworkName(&schedDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&schedDriver))
    .WillOnce(Return(ExecutorInfo("noexecutor", "")));

  EXPECT_CALL(sched, registered(&schedDriver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&schedDriver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&schedMessage),
                    Trigger(&schedFrameworkMessageCall)));

  schedDriver.start();

  WAIT_UNTIL(resourceOfferCall);

  ASSERT_GE(1, offers.size());

  vector<TaskDescription> tasks;
  tasks.push_back(TaskDescription(1, offers[0].slaveId, "", offers[0].params, ""));

  schedDriver.replyToOffer(offerId, tasks, map<string, string>());

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state);

  FrameworkMessage hello(offers[0].slaveId, 1, "hello");
  schedDriver.sendFrameworkMessage(hello);

  WAIT_UNTIL(execFrameworkMessageCall);

  EXPECT_EQ("hello", execMessage.data);

  FrameworkMessage reply(args.slaveId, 1, "reply");
  execDriver->sendFrameworkMessage(reply);

  WAIT_UNTIL(schedFrameworkMessageCall);

  EXPECT_EQ("reply", schedMessage.data);

  schedDriver.stop();
  schedDriver.join();

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}
