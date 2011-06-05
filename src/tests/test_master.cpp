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
  PID master = run_nexus(1, 2, 1 * Gigabyte, false, false);
  NoopScheduler sched(1);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_TRUE(sched.registeredCalled);
  EXPECT_EQ(1, sched.offersGotten);
  kill_nexus();
}


TEST(MasterTest, NoopFrameworkWithMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(10, 2, 1 * Gigabyte, false, false);
  NoopScheduler sched(10);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_TRUE(sched.registeredCalled);
  EXPECT_EQ(1, sched.offersGotten);
  kill_nexus();
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
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
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
  kill_nexus();
}


TEST(MasterTest, TooMuchMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = lexical_cast<string>(4 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  kill_nexus();
}


TEST(MasterTest, TooMuchCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "4";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  kill_nexus();
}


TEST(MasterTest, TooLittleCpuInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "0";
  params["mem"] = lexical_cast<string>(1 * Gigabyte);
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1073741824 MEM>", sched.errorMessage);
  kill_nexus();
}


TEST(MasterTest, TooLittleMemoryInTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
  vector<TaskDescription> tasks;
  map<string, string> params;
  params["cpus"] = "1";
  params["mem"] = "1";
  tasks.push_back(TaskDescription(1, "0-0", "", params, ""));
  FixedResponseScheduler sched(tasks);
  NexusSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <1 CPUs, 1 MEM>", sched.errorMessage);
  kill_nexus();
}


TEST(MasterTest, TooMuchMemoryAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
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
  kill_nexus();
}


TEST(MasterTest, TooMuchCpuAcrossTasks)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 3, 3 * Gigabyte, false, false);
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
  kill_nexus();
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(10, 2, 1 * Gigabyte, false, false);

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

  kill_nexus();
}


TEST(MasterTest, ResourcesReofferedAfterBadResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = run_nexus(1, 2, 1 * Gigabyte, false, false);

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

  kill_nexus();
}


class SlaveLostScheduler : public Scheduler
{
public:
  bool slaveLostCalled;
  
  SlaveLostScheduler() : slaveLostCalled(false) {}

  virtual ~SlaveLostScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "SlaveLostScheduler got a slot offer";
    vector<TaskDescription> tasks;
    d->replyToOffer(id, tasks, string_map());
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

  Slave s(master, Resources(2, 1 * Gigabyte), true);
  PID slave = Process::spawn(&s);

  SlaveLostScheduler sched;
  NexusSchedulerDriver driver(&sched, master);
  driver.start();

  Process::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  driver.join();

  // TODO(benh): Test lost slave due to missing heartbeats.

  EXPECT_TRUE(sched.slaveLostCalled);

  Process::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}
