#include <gtest/gtest.h>

#include <boost/lexical_cast.hpp>

#include "master.hpp"
#include "nexus_sched.hpp"
#include "nexus_local.hpp"

using std::string;
using std::vector;

using boost::lexical_cast;

using namespace nexus;
using namespace nexus::internal;

using nexus::internal::master::Master;
using nexus::internal::slave::Slave;


class NoopScheduler : public Scheduler
{
public:
  bool registeredCalled;
  int offersGotten;
  int slavesExpected;

public:
  NoopScheduler(int _slavesExpected)
    : slavesExpected(_slavesExpected),
      registeredCalled(false),
      offersGotten(0)
  {}

  virtual ~NoopScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "NoopScheduler registered";
    registeredCalled = true;
  }

  virtual void resourceOffer(SchedulerDriver *d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "NoopScheduler got a slot offer";
    offersGotten++;
    EXPECT_EQ(slavesExpected, offers.size());
    foreach (const SlaveOffer& offer, offers) {
      string cpus = offer.params.find("cpus")->second;
      string mem = offer.params.find("mem")->second;
      EXPECT_EQ("2", cpus);
      EXPECT_EQ(lexical_cast<string>(1 * Gigabyte), mem);
    }
    vector<TaskDescription> tasks;
    d->replyToOffer(id, tasks, string_map());
    d->stop();
  }
}; 


TEST(MasterTest, NoopFrameworkWithOneSlave)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);
  NoopScheduler sched(1);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_TRUE(sched.registeredCalled);
  EXPECT_EQ(1, sched.offersGotten);
  local::shutdown();
}


TEST(MasterTest, NoopFrameworkWithMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);
  NoopScheduler sched(10);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_TRUE(sched.registeredCalled);
  EXPECT_EQ(1, sched.offersGotten);
  local::shutdown();
}


class FixedResponseScheduler : public Scheduler
{
public:
  vector<TaskDescription> response;
  string errorMessage;
  
  FixedResponseScheduler(vector<TaskDescription> _response)
    : response(_response) {}

  virtual ~FixedResponseScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "FixedResponseScheduler got a slot offer";
    d->replyToOffer(id, response, string_map());
  }
  
  virtual void error(SchedulerDriver* d,
                     int code,
                     const std::string& message) {
    errorMessage = message;
    d->stop();
  }
};


TEST(MasterTest, DuplicateTaskIdsInResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "0-0", "", params, ""));
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Duplicate task ID: 1", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooMuchMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(4 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooMuchCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "4";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooLittleCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1073741824 MEM>", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooLittleMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = "1";
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <1 CPUs, 1 MEM>", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooMuchMemoryAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(2 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, TooMuchCpuAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "2";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  tasks.push_back(TaskDescription(2, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  NoopScheduler sched1(10);
  NexusSchedulerDriver driver1(&sched1, master);
  driver1.run();
  EXPECT_TRUE(sched1.registeredCalled);
  EXPECT_EQ(1, sched1.offersGotten);

  NoopScheduler sched2(10);
  NexusSchedulerDriver driver2(&sched2, master);
  driver2.run();
  EXPECT_TRUE(sched2.registeredCalled);
  EXPECT_EQ(1, sched2.offersGotten);

  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterBadResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched1(tasks);
  NexusSchedulerDriver driver1(&sched1, master);
  driver1.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1073741824 MEM>", sched1.errorMessage);

  NoopScheduler sched2(1);
  NexusSchedulerDriver driver2(&sched2, master);
  driver2.run();
  EXPECT_TRUE(sched2.registeredCalled);
  EXPECT_EQ(1, sched2.offersGotten);

  local::shutdown();
}


class SlaveLostScheduler : public Scheduler
{
public:
  PID slave;
  bool slaveLostCalled;
  
  SlaveLostScheduler(const PID &_slave)
    : slave(_slave), slaveLostCalled(false) {}

  virtual ~SlaveLostScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "SlaveLostScheduler got a slot offer";
    Process::post(slave, S2S_SHUTDOWN);
  }
  
  void slaveLost(SchedulerDriver* d, SlaveID slaveId) {
    slaveLostCalled = true;
    d->stop();
  }
};


TEST(MasterTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Slave s(Resources(2, 1 * Gigabyte), true);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  SlaveLostScheduler sched(slave);

  NexusSchedulerDriver driver(&sched, master);
  driver.run();

  EXPECT_TRUE(sched.slaveLostCalled);

  Process::wait(slave);

  Process::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}


/* TODO(benh): Test lost slave due to missing heartbeats. */


class FailoverScheduler : public Scheduler
{
public:
  bool registeredCalled;
  
  FailoverScheduler() : registeredCalled(false) {}

  virtual ~FailoverScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver *d, FrameworkID fid) {
    LOG(INFO) << "FailoverScheduler registered";
    registeredCalled = true;
    d->stop();
  }
};


class FailingScheduler : public Scheduler
{
public:
  FailoverScheduler *failoverSched;
  PID master;
  NexusSchedulerDriver *driver;
  string errorMessage;

  FailingScheduler(FailoverScheduler *_failoverSched, const PID &_master)
    : failoverSched(_failoverSched), master(_master) {}

  virtual ~FailingScheduler() {
    delete driver;
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "FailingScheduler registered";
    driver = new NexusSchedulerDriver(failoverSched, master, fid);
    driver->start();
  }

  virtual void error(SchedulerDriver* d,
                     int code,
                     const std::string& message) {
    errorMessage = message;
    d->stop();
  }
};


TEST(MasterTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  FailoverScheduler failoverSched;
  FailingScheduler failingSched(&failoverSched, master);

  NexusSchedulerDriver driver(&failingSched, master);
  driver.run();

  EXPECT_EQ("Framework failover", failingSched.errorMessage);

  failingSched.driver->join();

  EXPECT_TRUE(failoverSched.registeredCalled);

  local::shutdown();
}
