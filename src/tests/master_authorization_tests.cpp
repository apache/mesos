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

#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "master/allocator.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::AllocatorProcess;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;
using process::Promise;

using std::vector;

using testing::_;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Return;


class MasterAuthorizationTest : public MesosTest {};


// This test verifies that an authorized task launch is successful.
TEST_F(MasterAuthorizationTest, AuthorizedTask)
{
  // Setup ACLs so that the framework can launch tasks as "foo".
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values(DEFAULT_FRAMEWORK_INFO.principal());
  acl->mutable_users()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.acls = JSON::Protobuf(acls);

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Create an authorized executor.
  ExecutorInfo executor; // Bug in gcc 4.1.*, must assign on next line.
  executor = CREATE_EXECUTOR_INFO("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Create an authorized task.
  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that an unauthorized task launch is rejected.
TEST_F(MasterAuthorizationTest, UnauthorizedTask)
{
  // Setup ACLs so that no framework can launch as "foo".
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_users()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.acls = JSON::Protobuf(acls);

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Create an unauthorized executor.
  ExecutorInfo executor; // Bug in gcc 4.1.*, must assign on next line.
  executor = CREATE_EXECUTOR_INFO("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Create an unauthorized task.
  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that a 'killTask()' that comes before
// '_launchTasks()' is called results in TASK_KILLED.
TEST_F(MasterAuthorizationTest, KillTask)
{
  MockAuthorizer authorizer;
  Try<PID<Master> > master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Return a pending future from authorizer.
  Future<Nothing> future;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorize(An<const mesos::ACL::RunTasks&>()))
    .WillOnce(DoAll(FutureSatisfy(&future),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait until authorization is in progress.
  AWAIT_READY(future);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Now kill the task.
  driver.killTask(task.task_id());

  // Framework should get a TASK_KILLED right away.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  Future<Nothing> resourcesUnused =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesUnused);

  // Now complete authorization.
  promise.set(true);

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(resourcesUnused);

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that a slave removal that comes before
// '_launchTasks()' is called results in TASK_LOST.
TEST_F(MasterAuthorizationTest, SlaveRemoved)
{
  MockAuthorizer authorizer;
  Try<PID<Master> > master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Return a pending future from authorizer.
  Future<Nothing> future;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorize(An<const mesos::ACL::RunTasks&>()))
    .WillOnce(DoAll(FutureSatisfy(&future),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait until authorization is in progress.
  AWAIT_READY(future);

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now stop the slave.
  Stop(slave.get());

  AWAIT_READY(slaveLost);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> resourcesRecovered =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesRecovered);

  // Now complete authorization.
  promise.set(true);

  // Framework should get a TASK_LOST.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(resourcesRecovered);

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that a slave disconnection that comes before
// '_launchTasks()' is called results in TASK_LOST.
TEST_F(MasterAuthorizationTest, SlaveDisconnected)
{
  MockAuthorizer authorizer;
  Try<PID<Master> > master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Create a checkpointing slave so that a disconnected slave is not
  // immediately removed.
  slave::Flags flags = CreateSlaveFlags();
  flags.checkpoint = true;
  Try<PID<Slave> > slave = StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Return a pending future from authorizer.
  Future<Nothing> future;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorize(An<const mesos::ACL::RunTasks&>()))
    .WillOnce(DoAll(FutureSatisfy(&future),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait until authorization is in progress.
  AWAIT_READY(future);

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveDisconnected =
    FUTURE_DISPATCH(_, &AllocatorProcess::slaveDisconnected);

  // Now stop the slave.
  Stop(slave.get());

  AWAIT_READY(slaveDisconnected);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> resourcesRecovered =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesRecovered);

  // Now complete authorization.
  promise.set(true);

  // Framework should get a TASK_LOST.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(resourcesRecovered);

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that a framework removal that comes before
// '_launchTasks()' is called results in recovery of resources.
TEST_F(MasterAuthorizationTest, FrameworkRemoved)
{
  MockAuthorizer authorizer;
  Try<PID<Master> > master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Return a pending future from authorizer.
  Future<Nothing> future;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorize(An<const mesos::ACL::RunTasks&>()))
    .WillOnce(DoAll(FutureSatisfy(&future),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait until authorization is in progress.
  AWAIT_READY(future);

  Future<Nothing> frameworkRemoved =
    FUTURE_DISPATCH(_, &AllocatorProcess::frameworkRemoved);

  // Now stop the framework.
  driver.stop();
  driver.join();

  AWAIT_READY(frameworkRemoved);

  Future<Nothing> resourcesRecovered =
    FUTURE_DISPATCH(_, &AllocatorProcess::resourcesRecovered);

  // Now complete authorization.
  promise.set(true);

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(resourcesRecovered);

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that a reconciliation request that comes before
// '_launchTasks()' is ignored.
TEST_F(MasterAuthorizationTest, ReconcileTask)
{
  MockAuthorizer authorizer;
  Try<PID<Master> > master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Return a pending future from authorizer.
  Future<Nothing> future;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorize(An<const mesos::ACL::RunTasks&>()))
    .WillOnce(DoAll(FutureSatisfy(&future),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Wait until authorization is in progress.
  AWAIT_READY(future);

  // Scheduler shouldn't get an update from reconciliation.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  Future<ReconcileTasksMessage> reconcileTasksMessage =
    FUTURE_PROTOBUF(ReconcileTasksMessage(), _, _);

  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  status.set_state(TASK_STAGING);

  statuses.push_back(status);

  driver.reconcileTasks(statuses);

  AWAIT_READY(reconcileTasksMessage);

  // Make sure the framework doesn't receive any update.
  Clock::pause();
  Clock::settle();

  // Now stop the framework.
  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}
