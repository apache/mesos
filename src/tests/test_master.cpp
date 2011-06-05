#include <gtest/gtest.h>

#include <boost/lexical_cast.hpp>

#include <mesos_exec.hpp>
#include <mesos_sched.hpp>

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/isolation_module.hpp"
#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using std::string;
using std::vector;

using boost::lexical_cast;

using namespace mesos;
using namespace mesos::internal;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Framework;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;


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
    LOG(INFO) << "NoopScheduler registered with id " << fid;
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
    d->replyToOffer(id, tasks, map<string, string>());
    d->stop();
  }
}; 


TEST(MasterTest, NoopFrameworkWithOneSlave)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);
  NoopScheduler sched(1);
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
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
    d->replyToOffer(id, response, map<string, string>());
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
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1024 MEM>", sched.errorMessage);
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
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
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
  MesosSchedulerDriver driver(&sched, master);
  driver.run();
  EXPECT_EQ("Too many resources accepted", sched.errorMessage);
  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);
  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  NoopScheduler sched1(10);
  MesosSchedulerDriver driver1(&sched1, master);
  driver1.run();
  EXPECT_TRUE(sched1.registeredCalled);
  EXPECT_EQ(1, sched1.offersGotten);

  NoopScheduler sched2(10);
  MesosSchedulerDriver driver2(&sched2, master);
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
  MesosSchedulerDriver driver1(&sched1, master);
  driver1.run();
  EXPECT_EQ("Invalid task size: <0 CPUs, 1024 MEM>", sched1.errorMessage);

  NoopScheduler sched2(1);
  MesosSchedulerDriver driver2(&sched2, master);
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
    MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  }
  
  virtual void slaveLost(SchedulerDriver* d, SlaveID slaveId) {
    slaveLostCalled = true;
    d->stop();
  }
};


TEST(MasterTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;
  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  SlaveLostScheduler sched(slave);

  MesosSchedulerDriver driver(&sched, master);
  driver.run();

  EXPECT_TRUE(sched.slaveLostCalled);

  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}



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
  Scheduler *failover;
  PID master;
  MesosSchedulerDriver *driver;
  string errorMessage;

  FailingScheduler(Scheduler *_failover, const PID &_master)
    : failover(_failover), master(_master) {}

  virtual ~FailingScheduler() {
    delete driver;
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "FailingScheduler registered";
    driver = new MesosSchedulerDriver(failover, master, fid);
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

  MesosSchedulerDriver driver(&failingSched, master);
  driver.run();

  EXPECT_EQ("Framework failover", failingSched.errorMessage);

  failingSched.driver->join();

  EXPECT_TRUE(failoverSched.registeredCalled);

  local::shutdown();
}


class OfferRescindedScheduler : public Scheduler
{
public:
  const PID slave;
  bool offerRescindedCalled;
  
  OfferRescindedScheduler(const PID &_slave)
    : slave(_slave), offerRescindedCalled(false) {}

  virtual ~OfferRescindedScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "OfferRescindedScheduler got a slot offer";
    vector<TaskDescription> tasks;
    ASSERT_TRUE(offers.size() == 1);
    const SlaveOffer &offer = offers[0];
    TaskDescription desc(0, offer.slaveId, "", offer.params, "");
    tasks.push_back(desc);
    d->replyToOffer(id, tasks, map<string, string>());
    MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  }

  virtual void offerRescinded(SchedulerDriver* d, OfferID)
  {
    offerRescindedCalled = true;
    d->stop();
  }
};


class OfferReplyMessageFilter : public MessageFilter
{
public:
  virtual bool filter(struct msg *msg) {
    return msg->id == F2M_SLOT_OFFER_REPLY;
  }
};


TEST(MasterTest, OfferRescinded)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  OfferReplyMessageFilter filter;
  Process::filter(&filter);

  Master m;
  PID master = Process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;
  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  OfferRescindedScheduler sched(slave);
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_TRUE(sched.offerRescindedCalled);

  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);

  Process::filter(NULL);
}


class SlavePartitionedScheduler : public Scheduler
{
public:
  bool slaveLostCalled;
  
  SlavePartitionedScheduler()
    : slaveLostCalled(false) {}

  virtual ~SlavePartitionedScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void slaveLost(SchedulerDriver* d, SlaveID slaveId) {
    slaveLostCalled = true;
    d->stop();
  }
};


class HeartbeatMessageFilter : public MessageFilter
{
public:
  virtual bool filter(struct msg *msg) {
    return msg->id == SH2M_HEARTBEAT;
  }
};


TEST(MasterTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HeartbeatMessageFilter filter;
  Process::filter(&filter);

  ProcessClock::pause();

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  SlavePartitionedScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  driver.start();

  ProcessClock::advance(master::HEARTBEAT_TIMEOUT);

  driver.join();

  EXPECT_TRUE(sched.slaveLostCalled);

  local::shutdown();

  ProcessClock::resume();

  Process::filter(NULL);
}


class TaskRunningScheduler : public Scheduler
{
public:
  FrameworkID fid;
  bool statusUpdateCalled;
  string errorMessage;
  
  TaskRunningScheduler()
    : statusUpdateCalled(false) {}

  virtual ~TaskRunningScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "TaskRunningScheduler registered";
    this->fid = fid;
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "TaskRunningScheduler got a slot offer";
    vector<TaskDescription> tasks;
    ASSERT_TRUE(offers.size() == 1);
    const SlaveOffer &offer = offers[0];
    TaskDescription desc(0, offer.slaveId, "", offer.params, "");
    tasks.push_back(desc);
    d->replyToOffer(id, tasks, map<string, string>());
  }

  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    EXPECT_EQ(TASK_RUNNING, status.state);
    statusUpdateCalled = true;
    d->stop();
  }

  virtual void error(SchedulerDriver* d,
                     int code,
                     const std::string& message) {
    errorMessage = message;
    d->stop();
  }
};


class TaskRunningExecutor : public Executor {};


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


TEST(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  TaskRunningExecutor exec;
  LocalIsolationModule isolationModule(&exec);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  TaskRunningScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_TRUE(sched.statusUpdateCalled);
  EXPECT_EQ("", sched.errorMessage);

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}


class SchedulerFailoverStatusUpdateScheduler : public TaskRunningScheduler
{
 public:
  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    Process::filter(NULL);
    ProcessClock::advance(RELIABLE_TIMEOUT);
  }
};


class StatusUpdateFilter : public MessageFilter
{
public:
  TaskRunningScheduler *failover;
  TaskRunningScheduler *failing;
  const PID master;
  MesosSchedulerDriver *driver;

  StatusUpdateFilter(TaskRunningScheduler *_failover,
                     TaskRunningScheduler *_failing,
                     const PID &_master)
    : failover(_failover), failing(_failing), master(_master),
      driver(NULL) {}

  ~StatusUpdateFilter() {
    if (driver != NULL) {
      driver->join();
      delete driver;
      driver = NULL;
    }
  }

  virtual bool filter(struct msg *msg) {
    // TODO(benh): Fix the brokenness of this test due to blocking
    // S2M_FT_STATUS_UPDATE!
    if (driver == NULL &&
        msg->id == S2M_FT_STATUS_UPDATE &&
        !(msg->to == master)) {
      driver = new MesosSchedulerDriver(failover, master, failing->fid);
      driver->start();
      return true;
    }

    return false;
  }
};


TEST(MasterTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  ProcessClock::pause();

  Master m;
  PID master = Process::spawn(&m);

  TaskRunningExecutor exec;
  LocalIsolationModule isolationModule(&exec);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  SchedulerFailoverStatusUpdateScheduler failoverSched;
  TaskRunningScheduler failingSched;

  StatusUpdateFilter filter(&failoverSched, &failingSched, master);
  Process::filter(&filter);

  MesosSchedulerDriver driver(&failingSched, master);

  driver.run();

  EXPECT_FALSE(failingSched.statusUpdateCalled);
  EXPECT_EQ("Framework failover", failingSched.errorMessage);

  filter.driver->join();

  EXPECT_TRUE(failoverSched.statusUpdateCalled);
  EXPECT_EQ("", failoverSched.errorMessage);

  Process::filter(NULL);

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);

  ProcessClock::resume();
}


// An executor used in the framework message test that just sends a reply
// to each message received and logs the last message.
class FrameworkMessageExecutor : public Executor
{
public:
  bool messageReceived;
  string messageData;
  SlaveID mySlaveId;

  FrameworkMessageExecutor(): messageReceived(false) {}

  virtual ~FrameworkMessageExecutor() {}

  virtual void init(ExecutorDriver* d, const ExecutorArgs& args) {
    mySlaveId = args.slaveId;
  }

  virtual void frameworkMessage(ExecutorDriver* d, const FrameworkMessage& m) {
    LOG(INFO) << "FrameworkMessageExecutor got a message";
    messageReceived = true;
    messageData = m.data;
    // Send a message back to the scheduler, which will cause it to exit
    FrameworkMessage reply(mySlaveId, 0, "reply");
    d->sendFrameworkMessage(reply);
    LOG(INFO) << "Sent the reply back";
  }
};


// A scheduler used in the framework message test that launches a task, waits
// for it to start, sends it a framework message, and waits for a reply.
class FrameworkMessageScheduler : public Scheduler
{
public:
  FrameworkID fid;
  string errorMessage;
  bool messageReceived;
  string messageData;
  SlaveID slaveIdOfTask;

  FrameworkMessageScheduler() {}

  virtual ~FrameworkMessageScheduler() {}

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo("noexecutor", "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "FrameworkMessageScheduler registered";
    this->fid = fid;
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    LOG(INFO) << "FrameworkMessageScheduler got a slot offer";
    vector<TaskDescription> tasks;
    ASSERT_TRUE(offers.size() == 1);
    const SlaveOffer &offer = offers[0];
    TaskDescription desc(0, offer.slaveId, "", offer.params, "");
    tasks.push_back(desc);
    slaveIdOfTask = offer.slaveId;
    d->replyToOffer(id, tasks, map<string, string>());
  }


  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    EXPECT_EQ(TASK_RUNNING, status.state);
    LOG(INFO) << "Task is running; sending it a framework message";
    FrameworkMessage message(slaveIdOfTask, 0, "hello");
    d->sendFrameworkMessage(message);
  }


  virtual void frameworkMessage(SchedulerDriver* d, const FrameworkMessage& m) {
    LOG(INFO) << "FrameworkMessageScheduler got a message";
    messageReceived = true;
    messageData = m.data;
    // Stop our driver because the test is complete
    d->stop();
  }

  virtual void error(SchedulerDriver* d,
                     int code,
                     const std::string& message) {
    errorMessage = message;
    d->stop();
  }
};


// Tests that framework messages are sent correctly both in both the
// scheduler->executor direction and the executor->scheduler direction.
TEST(MasterTest, FrameworkMessages)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  FrameworkMessageExecutor exec;
  LocalIsolationModule isolationModule(&exec);

  Slave s(Resources(2, 1 * Gigabyte), true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  FrameworkMessageScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  driver.run();

  EXPECT_EQ("", sched.errorMessage);
  EXPECT_TRUE(exec.messageReceived);
  EXPECT_EQ("hello", exec.messageData);
  EXPECT_TRUE(sched.messageReceived);
  EXPECT_EQ("reply", sched.messageData);

  MesosProcess::post(slave, pack<S2S_SHUTDOWN>());
  Process::wait(slave);

  MesosProcess::post(master, pack<M2M_SHUTDOWN>());
  Process::wait(master);
}
