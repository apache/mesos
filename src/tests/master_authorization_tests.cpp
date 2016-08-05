// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/module/authorizer.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "master/master.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/detector/standalone.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"
#include "tests/utils.hpp"

namespace http = process::http;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Promise;

using process::http::Forbidden;
using process::http::OK;
using process::http::Response;

using std::string;
using std::vector;

using testing::_;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class MasterAuthorizationTest : public MesosTest {};


// This test verifies that an authorized task launch is successful.
TEST_F(MasterAuthorizationTest, AuthorizedTask)
{
  // Setup ACLs so that the framework can launch tasks as "foo".
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values(DEFAULT_FRAMEWORK_INFO.principal());
  acl->mutable_users()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Create an authorized executor.
  ExecutorInfo executor = CREATE_EXECUTOR_INFO("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that an unauthorized task launch is rejected.
TEST_F(MasterAuthorizationTest, UnauthorizedTask)
{
  // Setup ACLs so that no framework can launch as "foo".
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_users()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Create an unauthorized executor.
  ExecutorInfo executor = CREATE_EXECUTOR_INFO("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_UNAUTHORIZED, status.get().reason());

  driver.stop();
  driver.join();
}


// This test verifies that a 'killTask()' that comes before
// '_launchTasks()' is called results in TASK_KILLED.
TEST_F(MasterAuthorizationTest, KillTask)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Now kill the task.
  driver.killTask(task.task_id());

  // Framework should get a TASK_KILLED right away.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Now complete authorization.
  promise.set(true);

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(recoverResources);

  driver.stop();
  driver.join();
}


// This test verifies that a slave removal that comes before
// '_launchTasks()' is called results in TASK_LOST.
TEST_F(MasterAuthorizationTest, SlaveRemoved)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Stop the slave with explicit shutdown as otherwise with
  // checkpointing the master will wait for the slave to reconnect.
  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(slaveLost);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Now complete authorization.
  promise.set(true);

  // Framework should get a TASK_LOST.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status.get().source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, status.get().reason());

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(recoverResources);

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values.count(
                    "master/task_lost/source_master/reason_slave_removed"));
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u, stats.values["master/task_lost/source_master/reason_slave_removed"]);

  driver.stop();
  driver.join();
}


// This test verifies that a slave disconnection that comes before
// '_launchTasks()' is called results in TASK_LOST.
TEST_F(MasterAuthorizationTest, SlaveDisconnected)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::deactivateSlave);

  // Stop the slave with explicit shutdown message so that the master does not
  // wait for it to reconnect.
  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(deactivateSlave);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Now complete authorization.
  promise.set(true);

  // Framework should get a TASK_LOST.
  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status.get().source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, status.get().reason());

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(recoverResources);

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1u,
            stats.values.count(
                "master/task_lost/source_master/reason_slave_removed"));
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_master/reason_slave_removed"]);

  driver.stop();
  driver.join();
}


// This test verifies that a framework removal that comes before
// '_launchTasks()' is called results in recovery of resources.
TEST_F(MasterAuthorizationTest, FrameworkRemoved)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  // Now stop the framework.
  driver.stop();
  driver.join();

  AWAIT_READY(removeFramework);

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Now complete authorization.
  promise.set(true);

  // No task launch should happen resulting in all resources being
  // returned to the allocator.
  AWAIT_READY(recoverResources);
}


// This test verifies that two tasks each launched on a different
// slave with same executor id but different executor info are
// allowed even when the first task is pending due to authorization.
TEST_F(MasterAuthorizationTest, PendingExecutorInfoDiffersOnDifferentSlaves)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Start the first slave.
  MockExecutor exec1(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer1(&exec1);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer1);
  ASSERT_SOME(slave1);

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Launch the first task with the default executor id.
  ExecutorInfo executor1;
  executor1 = DEFAULT_EXECUTOR_INFO;
  executor1.mutable_command()->set_value("exit 1");

  TaskInfo task1 = createTask(
      offers1.get()[0], executor1.command().value(), executor1.executor_id());

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offers1.get()[0].id(), {task1});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Now start the second slave.
  MockExecutor exec2(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer2(&exec2);

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // Now launch the second task with the same executor id but
  // a different executor command.
  ExecutorInfo executor2;
  executor2 = executor1;
  executor2.mutable_command()->set_value("exit 2");

  TaskInfo task2 = createTask(
      offers2.get()[0], executor2.command().value(), executor2.executor_id());

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(Return(true));

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  // Complete authorization of 'task1'.
  promise.set(true);

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1.get().state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a framework registration with authorized
// role is successful.
TEST_F(MasterAuthorizationTest, AuthorizedRole)
{
  // Setup ACLs so that the framework can receive offers for role
  // "foo".
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values(DEFAULT_FRAMEWORK_INFO.principal());
  acl->mutable_roles()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.roles = "foo";
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that a framework registration with unauthorized
// role is denied.
TEST_F(MasterAuthorizationTest, UnauthorizedRole)
{
  // Setup ACLs so that no framework can receive offers for role
  // "foo".
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_roles()->add_values("foo");

  master::Flags flags = CreateMasterFlags();
  flags.roles = "foo";
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Framework should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();
}


// This test verifies that an authentication request that comes from
// the same instance of the framework (e.g., ZK blip) before
// 'Master::_registerFramework()' from an earlier attempt, causes the
// master to successfully register the framework.
TEST_F(MasterAuthorizationTest, DuplicateRegistration)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  // Create a detector for the scheduler driver because we want the
  // spurious leading master change to be known by the scheduler
  // driver only.
  StandaloneMasterDetector detector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Return pending futures from authorizer.
  Future<Nothing> authorize1;
  Promise<bool> promise1;
  Future<Nothing> authorize2;
  Promise<bool> promise2;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize1),
                    Return(promise1.future())))
    .WillOnce(DoAll(FutureSatisfy(&authorize2),
                    Return(promise2.future())))
    .WillRepeatedly(Return(true)); // Authorize subsequent registration retries.

  // Pause the clock to avoid registration retries.
  Clock::pause();

  driver.start();

  // Wait until first authorization attempt is in progress.
  AWAIT_READY(authorize1);

  // Simulate a spurious leading master change at the scheduler.
  detector.appoint(master.get()->pid);

  // Wait until second authorization attempt is in progress.
  AWAIT_READY(authorize2);

  // Now complete the first authorization attempt.
  promise1.set(true);

  // First registration request should succeed because the
  // framework PID did not change.
  AWAIT_READY(registered);

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  // Now complete the second authorization attempt.
  promise2.set(true);

  // Master should acknowledge the second registration attempt too.
  AWAIT_READY(frameworkRegisteredMessage);

  driver.stop();
  driver.join();
}


// This test verifies that an authentication request that comes from
// the same instance of the framework (e.g., ZK blip) before
// 'Master::_reregisterFramework()' from an earlier attempt, causes
// the master to successfully re-register the framework.
TEST_F(MasterAuthorizationTest, DuplicateReregistration)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  // Create a detector for the scheduler driver because we want the
  // spurious leading master change to be known by the scheduler
  // driver only.
  StandaloneMasterDetector detector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Return pending futures from authorizer after the first attempt.
  Future<Nothing> authorize2;
  Promise<bool> promise2;
  Future<Nothing> authorize3;
  Promise<bool> promise3;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(Return(true))
    .WillOnce(DoAll(FutureSatisfy(&authorize2),
                    Return(promise2.future())))
    .WillOnce(DoAll(FutureSatisfy(&authorize3),
                    Return(promise3.future())))
    .WillRepeatedly(Return(true)); // Authorize subsequent registration retries.

  // Pause the clock to avoid re-registration retries.
  Clock::pause();

  driver.start();

  // Wait for the framework to be registered.
  AWAIT_READY(registered);

  EXPECT_CALL(sched, disconnected(&driver));

  // Simulate a spurious leading master change at the scheduler.
  detector.appoint(master.get()->pid);

  // Wait until the second authorization attempt is in progress.
  AWAIT_READY(authorize2);

  // Simulate another spurious leading master change at the scheduler.
  detector.appoint(master.get()->pid);

  // Wait until the third authorization attempt is in progress.
  AWAIT_READY(authorize3);

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  // Now complete the second authorization attempt.
  promise2.set(true);

  // First re-registration request should succeed because the
  // framework PID did not change.
  AWAIT_READY(reregistered);

  Future<FrameworkReregisteredMessage> frameworkReregisteredMessage =
    FUTURE_PROTOBUF(FrameworkReregisteredMessage(), _, _);

  // Now complete the third authorization attempt.
  promise3.set(true);

  // Master should acknowledge the second re-registration attempt too.
  AWAIT_READY(frameworkReregisteredMessage);

  driver.stop();
  driver.join();
}


// This test ensures that a framework that is removed while
// authorization for registration is in progress is properly handled.
TEST_F(MasterAuthorizationTest, FrameworkRemovedBeforeRegistration)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())))
    .WillRepeatedly(Return(true)); // Authorize subsequent registration retries.

  // Pause the clock to avoid scheduler registration retries.
  Clock::pause();

  driver.start();

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  // Stop the framework.
  // At this point the framework is disconnected but the master does
  // not take any action because the framework is not in its map yet.
  driver.stop();
  driver.join();

  // Settle the clock here to ensure master handles the framework
  // 'exited' event.
  Clock::settle();
  Clock::resume();

  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  // Now complete authorization.
  promise.set(true);

  // When the master tries to link to a non-existent framework PID
  // it should realize the framework is gone and remove it.
  AWAIT_READY(removeFramework);
}


// This test ensures that a framework that is removed while
// authorization for re-registration is in progress is properly
// handled.
TEST_F(MasterAuthorizationTest, FrameworkRemovedBeforeReregistration)
{
  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  // Create a detector for the scheduler driver because we want the
  // spurious leading master change to be known by the scheduler
  // driver only.
  StandaloneMasterDetector detector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Return a pending future from authorizer after first attempt.
  Future<Nothing> authorize2;
  Promise<bool> promise2;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(Return(true))
    .WillOnce(DoAll(FutureSatisfy(&authorize2),
                    Return(promise2.future())));

  // Pause the clock to avoid scheduler registration retries.
  Clock::pause();

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(registered);

  EXPECT_CALL(sched, disconnected(&driver));

  // Framework should not be re-registered.
  EXPECT_CALL(sched, reregistered(&driver, _))
    .Times(0);

  // Simulate a spurious leading master change at the scheduler.
  detector.appoint(master.get()->pid);

  // Wait until the second authorization attempt is in progress.
  AWAIT_READY(authorize2);

  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  // Stop the framework.
  driver.stop();
  driver.join();

  // Wait until the framework is removed.
  AWAIT_READY(removeFramework);

  // Now complete the second authorization attempt.
  promise2.set(true);

  // Master should drop the second framework re-registration request
  // because the framework PID was removed from 'authenticated' map.
  // Settle the clock here to ensure 'Master::_reregisterFramework()'
  // is executed.
  Clock::settle();
}


template <typename T>
class MasterAuthorizerTest : public MesosTest {};


typedef ::testing::Types<
  LocalAuthorizer,
  tests::Module<Authorizer, TestLocalAuthorizer>>
  AuthorizerTypes;


TYPED_TEST_CASE(MasterAuthorizerTest, AuthorizerTypes);


// This test verifies that authorization based endpoint filtering
// works correctly on the /state-summary endpoint.
// Only one of the default users is allowed to view frameworks.
TYPED_TEST(MasterAuthorizerTest, FilterStateSummaryEndpoint)
{
  const string stateSummaryEndpoint = "state-summary";
  const string user = "bar";

  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Start framwork with user "bar".
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.set_user(user);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateSummaryEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object stateSummary = parse.get();

    ASSERT_TRUE(stateSummary.values["frameworks"].is<JSON::Array>());
    EXPECT_EQ(
        1u, stateSummary.values["frameworks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user not allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateSummaryEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object stateSummary = parse.get();

    ASSERT_TRUE(stateSummary.values["frameworks"].is<JSON::Array>());
    EXPECT_TRUE(
        stateSummary.values["frameworks"].as<JSON::Array>().values.empty());
  }

  driver.stop();
  driver.join();
}


// This test verifies that authorization based endpoint filtering
// works correctly on the /state endpoint.
// Both default users are allowed to to view high level frameworks, but only
// one is allowed to view the tasks.
TYPED_TEST(MasterAuthorizerTest, FilterStateEndpoint)
{
  const string stateEndpoint = "state";
  const string user = "bar";

  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see executors running under any user.
    ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see tasks running under any user.
    ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Start framwork with user "bar".
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.set_user(user);

  // Create an executor with user "bar".
  ExecutorInfo executor = CREATE_EXECUTOR_INFO("test-executor", "sleep 2");
  executor.mutable_command()->set_user(user);

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
    EXPECT_EQ(1u, state.values["frameworks"].as<JSON::Array>().values.size());

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_EQ(1u, framework.values["tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the executor.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();
    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_TRUE(framework.values["tasks"].as<JSON::Array>().values.empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}



// This test verifies that authorization based endpoint filtering
// works correctly on the /frameworks endpoint.
// Both default users are allowed to to view high level frameworks, but only
// one is allowed to view the tasks.
TYPED_TEST(MasterAuthorizerTest, FilterFrameworksEndpoint)
{
  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see executors running under any user.
    ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see tasks running under any user.
    ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Start framwork with user "bar".
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.set_user("bar");

  // Create an executor with user "bar".
  ExecutorInfo executor = CREATE_EXECUTOR_INFO("test-executor", "sleep 2");
  executor.mutable_command()->set_user("bar");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
    EXPECT_EQ(1u, state.values["frameworks"].as<JSON::Array>().values.size());

    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_EQ(1u, framework.values["tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the executor.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object state = parse.get();
    ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_TRUE(framework.values["tasks"].as<JSON::Array>().values.empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that authorization based endpoint filtering
// works correctly on the /tasks endpoint.
// Both default users are allowed to to view high level frameworks, but only
// one is allowed to view the tasks.
TYPED_TEST(MasterAuthorizerTest, FilterTasksEndpoint)
{
  const string tasksEndpoint = "tasks";
  const string user = "bar";

  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see executors running under any user.
    ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see tasks running under any user.
    ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Start framwork with user "bar".
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role");
  frameworkInfo.set_user(user);

  // Create an executor with user "bar".
  ExecutorInfo executor = CREATE_EXECUTOR_INFO("test-executor", "sleep 2");
  executor.mutable_command()->set_user(user);

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillRepeatedly(Return());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        tasksEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["tasks"].is<JSON::Array>());
    EXPECT_EQ(1u, tasks.values["tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the tasks.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        tasksEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["tasks"].is<JSON::Array>());
    EXPECT_TRUE(tasks.values["tasks"].as<JSON::Array>().values.empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TYPED_TEST(MasterAuthorizerTest, ViewFlags)
{
  ACLs acls;

  {
    // Default principal can see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_flags()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can not see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_flags()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // The default principal should be able to access the flags.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        "flags",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response.get().body;

    response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
    EXPECT_TRUE(1u <= state.values["flags"].as<JSON::Object>().values.size());
  }

  // The second default principal should not have access to the
  // /flags endpoint and get a filtered view of the /state one.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        "flags",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response)
        << response.get().body;

    response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
        << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    EXPECT_TRUE(state.values.find("flags") == state.values.end());
  }
}


// This test verifies that authorization based endpoint filtering
// works correctly on the /roles endpoint.
TYPED_TEST(MasterAuthorizerTest, FilterRolesEndpoint)
{
  ACLs acls;

  {
    // Default principal can see all roles.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see no roles.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  const string ROLE1 = "role1";
  const string ROLE2 = "role2";

  master::Flags flags = MesosTest::CreateMasterFlags();
  flags.roles = strings::join(",", ROLE1, ROLE2);

  Try<Owned<cluster::Master>> master = this->StartMaster(
      authorizer.get(),
      flags);

  ASSERT_SOME(master);

  const string rolesEndpoint = "roles";

  // Retrieve endpoint with the user allowed to view all roles.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        rolesEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["roles"].is<JSON::Array>());
    EXPECT_EQ(3u, tasks.values["roles"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user not allowed to view the roles.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        rolesEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["roles"].is<JSON::Array>());
    EXPECT_EQ(0u, tasks.values["roles"].as<JSON::Array>().values.size());
  }
}


// This test verifies that authorization based endpoint filtering
// works correctly on the /state endpoint with orphaned tasks.
// Both default users are allowed to to view high level frameworks, but only
// one is allowed to view the tasks.
TYPED_TEST(MasterAuthorizerTest, FilterOrphanedTasks)
{
  ACLs acls;

  {
    // Default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // Second default principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see frameworks running under any user.
    ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see executors running under any user.
    ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  {
    // Default principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(ACL::Entity::ANY);
  }

  {
    // No other principal can see tasks running under any user.
    ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_users()->set_type(ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Owned<cluster::Master>> master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = this->StartSlave(
      &detector, &containerizer);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING.

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Send an update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

    // Wait until TASK_RUNNING of the task is received.
  AWAIT_READY(statusUpdate);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // The master failover.
  master->reset();
  master = this->StartMaster(authorizer.get());
  ASSERT_SOME(master);

  // Simulate a new master detected event to the slave.
  detector.appoint(master.get()->pid);

  // The framework will not re-register with the new master as the
  // scheduler is bound to the old master pid.

  AWAIT_READY(slaveReregisteredMessage);

  const string stateEndpoint = "state";

  // Retrieve endpoint with the user allowed to view the framework and
  // tasks.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["orphan_tasks"].is<JSON::Array>());
    EXPECT_EQ(1u, tasks.values["orphan_tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the tasks.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
      << response.get().body;

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["orphan_tasks"].is<JSON::Array>());
    EXPECT_TRUE(tasks.values["orphan_tasks"].as<JSON::Array>().values.empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
