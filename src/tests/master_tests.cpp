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

#include "master/frameworks_manager.hpp"
#include "master/master.hpp"
#include "master/simple_allocator.hpp"

#include <process/dispatch.hpp>
#include <process/future.hpp>

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::FrameworksManager;
using mesos::internal::master::FrameworksStorage;

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


TEST(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall, resourcesChangedCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  EXPECT_CALL(isolationModule,
              resourcesChanged(_, _, Resources(offers[0].resources())))
    .WillOnce(Trigger(&resourcesChangedCall));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  WAIT_UNTIL(resourcesChangedCall);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(MasterTest, KillTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger killTaskCall, shutdownCall;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(Trigger(&killTaskCall));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;
  TaskStatus status;

  trigger resourceOffersCall, statusUpdateCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskID taskId;
  taskId.set_value("1");

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.killTask(taskId);

  WAIT_UNTIL(killTaskCall);

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(MasterTest, FrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  ExecutorDriver* execDriver;
  ExecutorArgs args;
  string execData;

  trigger execFrameworkMessageCall, shutdownCall;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver), SaveArg<1>(&args)));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdate(TASK_RUNNING));

  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(DoAll(SaveArg<1>(&execData),
                    Trigger(&execFrameworkMessageCall)));

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;
  TaskStatus status;
  string schedData;

  trigger resourceOffersCall, statusUpdateCall, schedFrameworkMessageCall;

  EXPECT_CALL(sched, registered(&schedDriver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(DoAll(SaveArg<3>(&schedData),
                    Trigger(&schedFrameworkMessageCall)));

  schedDriver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  schedDriver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  string hello = "hello";

  schedDriver.sendFrameworkMessage(offers[0].slave_id(),
				   DEFAULT_EXECUTOR_ID,
				   hello);

  WAIT_UNTIL(execFrameworkMessageCall);

  EXPECT_EQ(hello, execData);

  string reply = "reply";

  execDriver->sendFrameworkMessage(reply);

  WAIT_UNTIL(schedFrameworkMessageCall);

  EXPECT_EQ(reply, schedData);

  schedDriver.stop();
  schedDriver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST(MasterTest, MultipleExecutors)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SimpleAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec1;
  TaskDescription exec1Task;
  trigger exec1LaunchTaskCall, exec1ShutdownCall;

  EXPECT_CALL(exec1, init(_, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec1Task),
                    Trigger(&exec1LaunchTaskCall),
                    SendStatusUpdate(TASK_RUNNING)));

  EXPECT_CALL(exec1, shutdown(_))
    .WillOnce(Trigger(&exec1ShutdownCall));

  MockExecutor exec2;
  TaskDescription exec2Task;
  trigger exec2LaunchTaskCall, exec2ShutdownCall;

  EXPECT_CALL(exec2, init(_, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<1>(&exec2Task),
                    Trigger(&exec2LaunchTaskCall),
                    SendStatusUpdate(TASK_RUNNING)));

  EXPECT_CALL(exec2, shutdown(_))
    .WillOnce(Trigger(&exec2ShutdownCall));

  ExecutorID executorId1;
  executorId1.set_value("executor-1");

  ExecutorID executorId2;
  executorId2.set_value("executor-2");

  map<ExecutorID, Executor*> execs;
  execs[executorId1] = &exec1;
  execs[executorId2] = &exec2;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;
  TaskStatus status1, status2;

  trigger resourceOffersCall, statusUpdateCall1, statusUpdateCall2;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status1), Trigger(&statusUpdateCall1)))
    .WillOnce(DoAll(SaveArg<1>(&status2), Trigger(&statusUpdateCall2)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  ASSERT_NE(0, offers.size());

  TaskDescription task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task1.mutable_executor()->mutable_executor_id()->MergeFrom(executorId1);
  task1.mutable_executor()->set_uri("noexecutor");

  TaskDescription task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task2.mutable_executor()->mutable_executor_id()->MergeFrom(executorId2);
  task2.mutable_executor()->set_uri("noexecutor");

  vector<TaskDescription> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall1);

  EXPECT_EQ(TASK_RUNNING, status1.state());

  WAIT_UNTIL(statusUpdateCall2);

  EXPECT_EQ(TASK_RUNNING, status2.state());

  WAIT_UNTIL(exec1LaunchTaskCall);

  EXPECT_EQ(task1.task_id(), exec1Task.task_id());

  WAIT_UNTIL(exec2LaunchTaskCall);

  EXPECT_EQ(task2.task_id(), exec2Task.task_id());

  driver.stop();
  driver.join();

  WAIT_UNTIL(exec1ShutdownCall); // To ensure can deallocate MockExecutor.
  WAIT_UNTIL(exec2ShutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// FrameworksManager test cases.

class MockFrameworksStorage : public FrameworksStorage
{
public:
  // We need this typedef because MOCK_METHOD is a macro.
  typedef map<FrameworkID, FrameworkInfo> Map_FrameworkId_FrameworkInfo;

  MOCK_METHOD0(list, Future<Result<Map_FrameworkId_FrameworkInfo> >());
  MOCK_METHOD2(add, Future<Result<bool> >(const FrameworkID&,
                                          const FrameworkInfo&));
  MOCK_METHOD1(remove, Future<Result<bool> >(const FrameworkID&));
};


// This fixture sets up expectations on the storage class
// and spawns both storage and frameworks manager.
class FrameworksManagerTestFixture : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    ASSERT_TRUE(GTEST_IS_THREADSAFE);

    storage = new MockFrameworksStorage();
    process::spawn(storage);

    EXPECT_CALL(*storage, list())
      .WillOnce(Return(Result<map<FrameworkID, FrameworkInfo> >(infos)));

    EXPECT_CALL(*storage, add(_, _))
      .WillRepeatedly(Return(Result<bool>::some(true)));

    EXPECT_CALL(*storage, remove(_))
      .WillRepeatedly(Return(Result<bool>::some(true)));

    manager = new FrameworksManager(storage);
    process::spawn(manager);
  }

  virtual void TearDown()
  {
    process::terminate(manager);
    process::wait(manager);
    delete manager;

    process::terminate(storage);
    process::wait(storage);
    delete storage;
  }

  map<FrameworkID, FrameworkInfo> infos;

  MockFrameworksStorage* storage;
  FrameworksManager* manager;
};


TEST_F(FrameworksManagerTestFixture, AddFramework)
{
  // Test if initially FM returns empty list.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future.await(2.0));
  EXPECT_TRUE(future.get().get().empty());

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id, info);

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  // Check if framework manager returns the added framework.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future3 =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future3.await(2.0));

  map<FrameworkID, FrameworkInfo> result = future3.get().get();

  ASSERT_EQ(1, result.count(id));
  EXPECT_EQ("test name", result[id].name());
  EXPECT_EQ("test user", result[id].user());

  // Check if the framework exists.
  Future<Result<bool> > future4 =
    process::dispatch(manager, &FrameworksManager::exists, id);

  ASSERT_TRUE(future4.await(2.0));
  EXPECT_TRUE(future4.get().get());
}


TEST_F(FrameworksManagerTestFixture, RemoveFramework)
{
  Clock::pause();

  // Remove a non-existent framework.
  FrameworkID id;
  id.set_value("non-existent framework");

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, seconds(0));

  ASSERT_TRUE(future1.await(2.0));
  EXPECT_TRUE(future1.get().isError());

  // Remove an existing framework.

  // First add a dummy framework.
  FrameworkID id2;
  id2.set_value("id2");

  FrameworkInfo info2;
  info2.set_name("test name");
  info2.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id2, info2);

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  // Now remove the added framework.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id2, seconds(1.0));

  Clock::update(Clock::now(manager) + 1.0);

  ASSERT_TRUE(future3.await(2.0));
  EXPECT_TRUE(future2.get().get());

  // Now check if the removed framework exists...it shouldn't.
  Future<Result<bool> > future4 =
    process::dispatch(manager, &FrameworksManager::exists, id2);

  ASSERT_TRUE(future4.await(2.0));
  EXPECT_FALSE(future4.get().get());

  Clock::resume();
}


TEST_F(FrameworksManagerTestFixture, ResurrectFramework)
{
  // Resurrect a non-existent framework.
  FrameworkID id;
  id.set_value("non-existent framework");

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  ASSERT_TRUE(future1.await(2.0));
  EXPECT_FALSE(future1.get().get());

  // Resurrect an existent framework that is NOT being removed.
  // Add a dummy framework.
  FrameworkID id2;
  id2.set_value("id2");

  FrameworkInfo info2;
  info2.set_name("test name");
  info2.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id2, info2);

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::resurrect, id2);

  ASSERT_TRUE(future3.await(2.0));
  EXPECT_TRUE(future3.get().get());
}


// TODO(vinod): Using a paused clock in the tests means that
// future.await() may wait forever. This makes debugging hard.
TEST_F(FrameworksManagerTestFixture, ResurrectExpiringFramework)
{
  // This is the crucial test.
  // Resurrect an existing framework that is being removed,is being removed,
  // which should cause the remove to be unsuccessful.

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  process::dispatch(manager, &FrameworksManager::add, id, info);

  Clock::pause();

  // Remove after 2 secs.
  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, seconds(2.0));

  // Resurrect in the meanwhile.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  Clock::update(Clock::now(manager) + 2.0);

  ASSERT_TRUE(future1.await(2.0));
  EXPECT_FALSE(future1.get().get());

  Clock::resume();
}


TEST_F(FrameworksManagerTestFixture, ResurrectInterspersedExpiringFrameworks)
{
  // This is another crucial test.
  // Two remove messages are interspersed with a resurrect.
  // Only the second remove should actually remove the framework.

  // Add a dummy framework.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  process::dispatch(manager, &FrameworksManager::add, id, info);

  Clock::pause();

  Future<Result<bool> > future1 =
    process::dispatch(manager, &FrameworksManager::remove, id, seconds(2.0));

  // Resurrect in the meanwhile.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::resurrect, id);

  // Remove again.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id, seconds(1.0));

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  Clock::update(Clock::now(manager) + 1.0);

  ASSERT_TRUE(future3.await(2.0));
  EXPECT_TRUE(future3.get().get());

  Clock::update(Clock::now(manager) + 2.0);

  ASSERT_TRUE(future1.await(2.0));
  EXPECT_FALSE(future1.get().get());

  Clock::resume();
}


// Not deriving from fixture...because we want to set specific expectations.
// Specifically we simulate caching failure in FrameworksManager.
TEST(FrameworksManagerTest, CacheFailure)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockFrameworksStorage storage;
  process::spawn(storage);

  Result<map<FrameworkID, FrameworkInfo> > errMsg =
    Result<map<FrameworkID, FrameworkInfo> >::error("Fake Caching Error.");

  EXPECT_CALL(storage, list())
    .Times(2)
    .WillRepeatedly(Return(errMsg));

  EXPECT_CALL(storage, add(_, _))
    .WillOnce(Return(Result<bool>::some(true)));

  EXPECT_CALL(storage, remove(_))
    .Times(0);

  FrameworksManager manager(&storage);
  process::spawn(manager);

  // Test if initially FrameworksManager returns error.
  Future<Result<map<FrameworkID, FrameworkInfo> > > future1 =
    process::dispatch(manager, &FrameworksManager::list);

  ASSERT_TRUE(future1.await(2.0));
  ASSERT_TRUE(future1.get().isError());
  EXPECT_EQ(future1.get().error(), "Error caching framework infos.");

  // Add framework should function normally despite caching failure.
  FrameworkID id;
  id.set_value("id");

  FrameworkInfo info;
  info.set_name("test name");
  info.set_user("test user");

  // Add the framework.
  Future<Result<bool> > future2 =
    process::dispatch(manager, &FrameworksManager::add, id, info);

  ASSERT_TRUE(future2.await(2.0));
  EXPECT_TRUE(future2.get().get());

  // Remove framework should fail due to caching failure.
  Future<Result<bool> > future3 =
    process::dispatch(manager, &FrameworksManager::remove, id, seconds(0));

  ASSERT_TRUE(future3.await(2.0));
  ASSERT_TRUE(future3.get().isError());
  EXPECT_EQ(future3.get().error(), "Error caching framework infos.");

  process::terminate(manager);
  process::wait(manager);

  process::terminate(storage);
  process::wait(storage);
}
