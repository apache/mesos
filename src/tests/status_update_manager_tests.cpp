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

#include <map>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/paths.hpp"
#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "messages/messages.hpp"

#include "tests/filter.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;
using namespace mesos::internal::slave::paths;

using namespace process;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using std::list;
using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::Return;
using testing::SaveArg;


class StatusUpdateManagerTest: public ::testing::Test
{
protected:
  static void SetUpTestCase()
  {
    flags.checkpoint = true;
    flags.work_dir = "/tmp/mesos-tests";
    os::rmdir(flags.work_dir);
  }

  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    a = new Allocator(&allocator);
    m = new Master(a, &files);
    master = process::spawn(m);

    execs[DEFAULT_EXECUTOR_ID] = &exec;

    isolator = new TestingIsolator(execs);

    s = new Slave(flags, true, isolator, &files);
    slave = process::spawn(s);

    detector = new BasicMasterDetector(master, slave, true);

    frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_checkpoint(true); // Enable checkpointing.
  }

  virtual void TearDown()
  {
    delete detector;

    process::terminate(slave);
    process::wait(slave);
    delete s;

    delete isolator;
    process::terminate(master);
    process::wait(master);
    delete m;
    delete a;

    os::rmdir(flags.work_dir);
  }

  vector<TaskInfo> createTasks(const Offer& offer)
  {
    TaskInfo task;
    task.set_name("test-task");
    task.mutable_task_id()->set_value("1");
    task.mutable_slave_id()->MergeFrom(offer.slave_id());
    task.mutable_resources()->MergeFrom(offer.resources());
    task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

    vector<TaskInfo> tasks;
    tasks.push_back(task);

    return tasks;
  }

  HierarchicalDRFAllocatorProcess allocator;
  Allocator *a;
  Master* m;
  TestingIsolator* isolator;
  Slave* s;
  Files files;
  BasicMasterDetector* detector;
  FrameworkInfo frameworkInfo;
  MockExecutor exec;
  map<ExecutorID, Executor*> execs;
  MockScheduler sched;
  TaskStatus status;
  PID<Master> master;
  PID<Slave> slave;
  static slave::Flags flags;
};

// Initialize static members here.
slave::Flags StatusUpdateManagerTest::flags;


TEST_F(StatusUpdateManagerTest, CheckpointStatusUpdate)
{
  // Message expectations.
  trigger statusUpdateAckMsg;
  EXPECT_MESSAGE(Eq(StatusUpdateAcknowledgementMessage().GetTypeName()),
                 _,
                 slave)
    .WillRepeatedly(DoAll(Trigger(&statusUpdateAckMsg), Return(false)));

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Trigger(&shutdownCall));

  // Scheduler expectations.
  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return());

  MesosSchedulerDriver driver(&sched, frameworkInfo, master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0u, offers.size());
  driver.launchTasks(offers[0].id(), createTasks(offers[0]));

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  // Check if the status update was properly checkpointed.
  WAIT_UNTIL(statusUpdateAckMsg);

  sleep(1); // To make sure the status updates manager acted on the ACK.

  // Ensure that both the status update and its acknowledgement
  // are correctly checkpointed.
  Try<list<string> > found = os::find(flags.work_dir, TASK_UPDATES_FILE);
  ASSERT_SOME(found);
  ASSERT_EQ(1u, found.get().size());

  Try<int> fd = os::open(found.get().front(), O_RDONLY);
  ASSERT_SOME(fd);

  int updates = 0;
  int acks = 0;
  string uuid;
  Result<StatusUpdateRecord> record = None();
  while (true) {
    record = ::protobuf::read<StatusUpdateRecord>(fd.get());

    if (!record.isSome()) {
      break;
    }

    if (record.get().type() == StatusUpdateRecord::UPDATE) {
      EXPECT_EQ(TASK_RUNNING, record.get().update().status().state());
      uuid = record.get().update().uuid();
      updates++;
    } else {
      EXPECT_EQ(uuid, record.get().uuid());
      acks++;
    }
  }

  ASSERT_TRUE(record.isNone());
  ASSERT_EQ(1, updates);
  ASSERT_EQ(1, acks);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.
}


TEST_F(StatusUpdateManagerTest, RetryStatusUpdate)
{
  // Message expectations.

  // Drop the first status update message.
  trigger statusUpdateMsgDrop;
  EXPECT_MESSAGE(Eq(StatusUpdateMessage().GetTypeName()), master, _)
    .WillOnce(DoAll(Trigger(&statusUpdateMsgDrop), Return(true)))
    .WillRepeatedly(Return(false));

  // Executor expectations.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(SendStatusUpdateFromTask(TASK_RUNNING));

  trigger shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Trigger(&shutdownCall));

  // Scheduler expectations.
  EXPECT_CALL(sched, registered(_, _, _))
    .Times(1);

  trigger resourceOffersCall;
  vector<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers), Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  trigger statusUpdateCall;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)))
    .WillRepeatedly(Return());

  MesosSchedulerDriver driver(&sched, frameworkInfo, master);

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  Clock::pause();

  EXPECT_NE(0u, offers.size());
  driver.launchTasks(offers[0].id(), createTasks(offers[0]));

  WAIT_UNTIL(statusUpdateMsgDrop);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL.secs());

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  Clock::resume();

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // Ensures MockExecutor can be deallocated.
}
