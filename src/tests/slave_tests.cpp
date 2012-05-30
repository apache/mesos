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

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include "common/time.hpp"

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/master.hpp"
#include "master/simple_allocator.hpp"

#include "slave/constants.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;
using mesos::internal::master::SimpleAllocator;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;

class SlaveTest : public ::testing::Test
{
protected:
  static void SetUpTestCase()
  {
    conf.set("MESOS_WORK_DIR", "/tmp/mesos");
    workDir = conf["work_dir"];
  }

  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    process::filter(&filter);

    EXPECT_MESSAGE(filter, _, _, _)
      .WillRepeatedly(Return(false));

    a = new SimpleAllocator();
    m = new Master(a);
    master = process::spawn(m);

    execs[DEFAULT_EXECUTOR_ID] = &exec;

    EXPECT_CALL(exec, registered(_, _, _, _))
      .WillRepeatedly(Return());

    EXPECT_CALL(exec, launchTask(_, _))
      .WillRepeatedly(SendStatusUpdate(TASK_RUNNING));

    EXPECT_CALL(exec, shutdown(_))
      .WillRepeatedly(Return());

    driver = new MesosSchedulerDriver(&sched, DEFAULT_FRAMEWORK_INFO, master);
  }

  virtual void TearDown()
  {
    stopSlave();

    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;

    process::filter(NULL);

    utils::os::rmdir(workDir);
  }

  void startSlave()
  {
    isolationModule = new TestingIsolationModule(execs);

    s = new Slave(conf, true, isolationModule);
    slave = process::spawn(s);

    detector = new BasicMasterDetector(master, slave, true);
  }

  void stopSlave()
  {
    delete detector;

    process::terminate(slave);
    process::wait(slave);
    delete s;

    delete isolationModule;
  }

  void restartSlave()
  {
    stopSlave();
    startSlave();
  }

  // Launches a task based on the received offer.
  void launchTask(const ExecutorInfo& executorInfo)
  {
    EXPECT_NE(0, offers.size());

    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value(UUID::random().toString());
    task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
    task.mutable_resources()->MergeFrom(offers[0].resources());
    task.mutable_executor()->MergeFrom(executorInfo);

    tasks.push_back(task);

    driver->launchTasks(offers[0].id(), tasks);

    tasks.clear();
  }

  void launchTask()
  {
    launchTask(DEFAULT_EXECUTOR_INFO);
  }

  SimpleAllocator* a;
  Master* m;
  TestingIsolationModule* isolationModule;
  Slave* s;
  BasicMasterDetector* detector;
  MockExecutor exec, exec1;
  map<ExecutorID, Executor*> execs;
  MockScheduler sched;
  MesosSchedulerDriver* driver;
  trigger resourceOffersCall;
  vector<Offer> offers;
  TaskStatus status;
  vector<TaskInfo> tasks;
  MockFilter filter;
  PID<Master> master;
  PID<Slave> slave;
  static Configuration conf;
  static string workDir;
};


// Initialize static members here.
Configuration SlaveTest::conf;
string SlaveTest::workDir;


TEST_F(SlaveTest, GarbageCollectSlaveDirs)
{
  trigger slaveRegisteredMsg;
  process::Message message;

  EXPECT_MESSAGE(filter, Eq(SlaveRegisteredMessage().GetTypeName()), _, _)
    .WillRepeatedly(DoAll(
        SaveArgField<0>(&process::MessageEvent::message, &message),
        Trigger(&slaveRegisteredMsg),
        Return(false)));

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)));

  trigger statusUpdateCall, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillOnce(Return()) // Ignore the TASK_LOST update.
    .WillOnce(Trigger(&statusUpdateCall2));

  EXPECT_CALL(sched, slaveLost(_, _))
    .Times(1);

  // Start the slave.
  startSlave();

  driver->start();

  WAIT_UNTIL(resourceOffersCall);
  launchTask();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  SlaveRegisteredMessage registeredMsg;

  WAIT_UNTIL(slaveRegisteredMsg); // Capture the slave id.
  registeredMsg.ParseFromString(message.body);
  SlaveID slaveId = registeredMsg.slave_id();

  // Make sure directory exists.
  const std::string& slaveDir = workDir + "/slaves/" + slaveId.value();
  ASSERT_TRUE(utils::os::exists(slaveDir));

  Clock::pause();

  // Advance the clock and restart the slave.
  stopSlave();

  hours timeout(slave::GC_TIMEOUT_HOURS);
  Clock::advance(timeout.secs());

  // Reset the triggers.
  slaveRegisteredMsg.value = false;
  resourceOffersCall.value = false;

  startSlave();

  WAIT_UNTIL(slaveRegisteredMsg); // Capture the new slave id.
  registeredMsg.ParseFromString(message.body);
  SlaveID slaveId2 = registeredMsg.slave_id();

  WAIT_UNTIL(resourceOffersCall);
  launchTask();

  WAIT_UNTIL(statusUpdateCall2);

  // By this time the old slave directory should be cleaned up and
  // the new directory should exist.
  ASSERT_FALSE(utils::os::exists(slaveDir));
  ASSERT_TRUE(utils::os::exists(workDir + "/slaves/" + slaveId2.value()));

  Clock::resume();

  driver->stop();
  driver->join();
}


TEST_F(SlaveTest, GarbageCollectExecutorDir)
{
  trigger runTaskMsg;
  process::Message message;

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)));

  trigger statusUpdateCall, statusUpdateCall2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillOnce(Return()) // Ignore the TASK_LOST update.
    .WillRepeatedly(Trigger(&statusUpdateCall));

  // Start the slave.
  startSlave();

  driver->start();

  WAIT_UNTIL(resourceOffersCall);
  launchTask();

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  const std::string executorDir =
      isolationModule->directories[DEFAULT_EXECUTOR_ID];

  ASSERT_TRUE(utils::os::exists(executorDir));

  statusUpdateCall.value = false;
  resourceOffersCall.value = false;

  // Kill the executor and inform the slave.
  isolationModule->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);
  process::dispatch(slave, &Slave::executorExited, frameworkId,
                    DEFAULT_EXECUTOR_ID, 0);

  statusUpdateCall.value = false;
  WAIT_UNTIL(resourceOffersCall);
  launchTask();
  WAIT_UNTIL(statusUpdateCall); // TASK_RUNNING

  Clock::pause();

  hours timeout(slave::GC_TIMEOUT_HOURS);
  Clock::advance(timeout.secs());

  // Kill the new executor and inform the slave.
  // We do this to make sure we can capture an event (TASK_LOST) that tells us
  // that the slave has processed its previous message (garbageCollect).
  isolationModule->killExecutor(frameworkId, DEFAULT_EXECUTOR_ID);
  process::dispatch(slave, &Slave::executorExited, frameworkId,
                    DEFAULT_EXECUTOR_ID, 0);

  statusUpdateCall.value = false;
  WAIT_UNTIL(resourceOffersCall);
  launchTask();
  WAIT_UNTIL(statusUpdateCall); // TASK_LOST

  // First executor's directory should be gced by now.
  ASSERT_FALSE(utils::os::exists(executorDir));

  Clock::resume();

  driver->stop();
  driver->join();
}
