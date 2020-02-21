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

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/module/authorizer.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "authorizer/local/authorizer.hpp"

#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/master.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/detector/standalone.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"
#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"
#include "tests/utils.hpp"

namespace http = process::http;

using std::shared_ptr;
using std::weak_ptr;

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::slave::Slave;

using mesos::v1::master::Call;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;

using process::http::Accepted;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;

using std::string;
using std::vector;

using testing::_;
using testing::AllOf;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Ne;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::Return;
using testing::Truly;

namespace mesos {
namespace internal {
namespace tests {

class MasterAuthorizationTest : public MesosTest
{
protected:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifndef __WINDOWS__
    // We don't need to actually launch tasks as the specified
    // user, since we are only interested in testing the
    // authorization path.
    flags.switch_user = false;
#endif

    return flags;
  }
};


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
  ExecutorInfo executor = createExecutorInfo("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Create an authorized task.
  TaskInfo task;
  task.set_name("test");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(executor);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

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
  ExecutorInfo executor = createExecutorInfo("test-executor", "exit 1");
  executor.mutable_command()->set_user("foo");

  MockExecutor exec(executor.executor_id());
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

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
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_UNAUTHORIZED, status->reason());

  // Make sure the task is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test verifies that even if one of the tasks in a task group is
// unauthorized, all the tasks in the task group are rejected.
TEST_F(MasterAuthorizationTest, UnauthorizedTaskGroup)
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

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("E");
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  executor.mutable_resources()->CopyFrom(resources);

  // Create an unauthorized task.
  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(resources);
  task1.mutable_command()->set_value("echo hello");
  task1.mutable_command()->set_user("foo");

  // Create an authorized task.
  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(resources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_UNAUTHORIZED, task1Status->reason());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_UNAUTHORIZED, task2Status->reason());

  // Make sure the task group is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

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
  frameworkInfo.set_roles(0, "foo");

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
  frameworkInfo.set_roles(0, "foo");

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

static shared_ptr<const ObjectApprover> getAcceptingObjectApprover()
{
  return std::make_shared<AcceptingObjectApprover>();
}


// This test verifies that disconnected frameworks do not own
// `ObjectApprover`s.
TEST_F(MasterAuthorizationTest, ObjectApproversDeletedOnDisconnection)
{
  shared_ptr<const ObjectApprover> approver = getAcceptingObjectApprover();

  MockAuthorizer authorizer;
  const weak_ptr<const ObjectApprover> weakApprover {approver};

  EXPECT_CALL(authorizer, getApprover(_, _))
    .WillRepeatedly(InvokeWithoutArgs([weakApprover]() {
      auto approver = weakApprover.lock();
      CHECK(approver);
      return approver;
    }));

  const Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(2).secs());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Disconnect framework but do not tear it down.
  driver.stop(true);
  driver.join();

  Clock::pause();
  Clock::settle();

  // Make sure that the test itself doesn't store approver anymore.
  approver.reset();

  ASSERT_TRUE(weakApprover.expired());
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
  Promise<shared_ptr<const ObjectApprover>> promise1;
  Future<Nothing> authorize2;
  Promise<shared_ptr<const ObjectApprover>> promise2;

  // Expect requests for two approvers for REGISTER_FRAMEWORK.
  EXPECT_CALL(authorizer, getApprover(_, authorization::REGISTER_FRAMEWORK))
    .WillOnce(DoAll(FutureSatisfy(&authorize1), Return(promise1.future())))
    .WillOnce(DoAll(FutureSatisfy(&authorize2), Return(promise2.future())));

  // Handle requests for all other approvers.
  EXPECT_CALL(authorizer, getApprover(_, Ne(authorization::REGISTER_FRAMEWORK)))
    .WillRepeatedly(Return(getAcceptingObjectApprover()));

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
  promise1.set(getAcceptingObjectApprover());

  // First registration request should succeed because the
  // framework PID did not change.
  AWAIT_READY(registered);

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  // Now complete the second authorization attempt.
  promise2.set(getAcceptingObjectApprover());

  // Master should acknowledge the second registration attempt too.
  AWAIT_READY(frameworkRegisteredMessage);

  driver.stop();
  driver.join();
}


// This test verifies that an authentication request that comes from
// the same instance of the framework (e.g., ZK blip) before
// 'Master::_reregisterFramework()' from an earlier attempt, causes
// the master to successfully reregister the framework.
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
  Promise<shared_ptr<const ObjectApprover>> promise2;
  Future<Nothing> authorize3;
  Promise<shared_ptr<const ObjectApprover>> promise3;
  EXPECT_CALL(authorizer, getApprover(_, authorization::REGISTER_FRAMEWORK))
    .WillOnce(Return(getAcceptingObjectApprover()))
    .WillOnce(DoAll(FutureSatisfy(&authorize2), Return(promise2.future())))
    .WillOnce(DoAll(FutureSatisfy(&authorize3), Return(promise3.future())));

  EXPECT_CALL(authorizer, getApprover(_, Ne(authorization::REGISTER_FRAMEWORK)))
    .WillRepeatedly(Return(getAcceptingObjectApprover()));

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
  promise2.set(getAcceptingObjectApprover());

  // First re-registration request should succeed because the
  // framework PID did not change.
  AWAIT_READY(reregistered);

  Future<FrameworkReregisteredMessage> frameworkReregisteredMessage =
    FUTURE_PROTOBUF(FrameworkReregisteredMessage(), _, _);

  // Now complete the third authorization attempt.
  promise3.set(getAcceptingObjectApprover());

  // Master should acknowledge the second re-registration attempt too.
  AWAIT_READY(frameworkReregisteredMessage);

  driver.stop();
  driver.join();
}


// This test ensures that a framework that is removed while
// obtaining ObjectApprovers during registration is properly handled.
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
  Promise<shared_ptr<const ObjectApprover>> promise;
  EXPECT_CALL(authorizer, getApprover(_, authorization::REGISTER_FRAMEWORK))
    .WillRepeatedly(DoAll(FutureSatisfy(&authorize), Return(promise.future())));

  EXPECT_CALL(authorizer, getApprover(_, Ne(authorization::REGISTER_FRAMEWORK)))
    .WillRepeatedly(Return(getAcceptingObjectApprover()));


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

  // Now make all the returned approvers ready.
  promise.set(getAcceptingObjectApprover());

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

  // Return a pending future from authorizer after first request for
  // REGISTER_FRAMEWORK approver
  Future<Nothing> authorize2;
  Promise<shared_ptr<const ObjectApprover>> promise2;
  EXPECT_CALL(authorizer, getApprover(_, authorization::REGISTER_FRAMEWORK))
    .WillOnce(Return(getAcceptingObjectApprover()))
    .WillOnce(DoAll(FutureSatisfy(&authorize2), Return(promise2.future())));

  // Handle all other actions.
  EXPECT_CALL(authorizer, getApprover(_, Ne(authorization::REGISTER_FRAMEWORK)))
    .WillRepeatedly(Return(getAcceptingObjectApprover()));

  // Pause the clock to avoid scheduler registration retries.
  Clock::pause();

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(registered);

  EXPECT_CALL(sched, disconnected(&driver));

  // Framework should not be reregistered.
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
  promise2.set(getAcceptingObjectApprover());

  // Master should drop the second framework re-registration request
  // because the framework PID was removed from 'authenticated' map.
  // Settle the clock here to ensure 'Master::_reregisterFramework()'
  // is executed.
  Clock::settle();
}


template <typename T>
class MasterAuthorizerTest : public MesosTest
{
protected:
  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifndef __WINDOWS__
    // We don't need to actually launch tasks as the specified
    // user, since we are only interested in testing the
    // authorization path.
    flags.switch_user = false;
#endif

    return flags;
  }
};


typedef ::testing::Types<
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    tests::Module<Authorizer, TestLocalAuthorizer>,
#endif // __WINDOWS__
    LocalAuthorizer> AuthorizerTypes;


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
  frameworkInfo.set_roles(0, "role");
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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
  frameworkInfo.set_roles(0, "role");
  frameworkInfo.set_user(user);

  // Create an executor with user "bar".
  ExecutorInfo executor = createExecutorInfo("test-executor", "sleep 2");
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
  ASSERT_FALSE(offers->empty());

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
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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
  frameworkInfo.set_roles(0, "role");
  frameworkInfo.set_user("bar");

  // Create an executor with user "bar".
  ExecutorInfo executor = createExecutorInfo("test-executor", "sleep 2");
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
  ASSERT_FALSE(offers->empty());

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
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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
  frameworkInfo.set_roles(0, "role");
  frameworkInfo.set_user(user);

  // Create an executor with user "bar".
  ExecutorInfo executor = createExecutorInfo("test-executor", "sleep 2");
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
  ASSERT_FALSE(offers->empty());

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
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Retrieve endpoint with the user allowed to view the framework.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        tasksEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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
    // Second default principal cannot see the flags.
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

    response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();
    ASSERT_TRUE(tasks.values["roles"].is<JSON::Array>());
    EXPECT_TRUE(tasks.values["roles"].as<JSON::Array>().values.empty());
  }
}


// This test verifies that authorization based endpoint filtering
// works correctly on the /state endpoint with orphaned tasks.  Both
// default users are allowed to to view high level frameworks, but
// only one is allowed to view the tasks. Although the tasks are
// "orphaned" (in the sense that their framework has not yet
// reregistered), they are now reported under the "tasks" key.
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

  // The framework will not reregister with the new master as the
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

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();

    // The "orphan_tasks" array should be empty, regardless of authorization.
    ASSERT_TRUE(tasks.values["orphan_tasks"].is<JSON::Array>());
    EXPECT_TRUE(tasks.values["orphan_tasks"].as<JSON::Array>().values.empty());

    ASSERT_TRUE(tasks.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = tasks.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_EQ(1u, framework.values["tasks"].as<JSON::Array>().values.size());
  }

  // Retrieve endpoint with the user allowed to view the framework,
  // but not the tasks.
  {
    Future<Response> response = http::get(
        master.get()->pid,
        stateEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Object tasks = parse.get();

    // The "orphan_tasks" array should be empty, regardless of authorization.
    ASSERT_TRUE(tasks.values["orphan_tasks"].is<JSON::Array>());
    EXPECT_TRUE(tasks.values["orphan_tasks"].as<JSON::Array>().values.empty());

    ASSERT_TRUE(tasks.values["frameworks"].is<JSON::Array>());

    JSON::Array frameworks = tasks.values["frameworks"].as<JSON::Array>();
    EXPECT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();
    EXPECT_TRUE(framework.values["tasks"].as<JSON::Array>().values.empty());
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(MasterAuthorizationTest, AuthorizedToRegisterAndReregisterAgent)
{
  // Set up ACLs so that the agent can (re)register.
  ACLs acls;
  mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_agents()->set_type(ACL::Entity::ANY);

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Simulate a recovered agent and verify that it is allowed to reregister.
  slave->reset();

  Future<Message> slaveReregisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveReregisteredMessage().GetTypeName()), _, _);

  slave = StartSlave(detector.get(), slaveFlags);

  AWAIT_READY(slaveReregisteredMessage);
}


// This test verifies that an agent without the right ACLs
// is not allowed to register and is not shut down.
TEST_F(MasterAuthorizationTest, UnauthorizedToRegisterAgent)
{
  Clock::pause();

  // Set up ACLs that disallows the agent's principal to register.
  ACLs acls;
  mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_agents()->set_type(ACL::Entity::NONE);

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Previously, agents were shut down when registration failed due to
  // authorization. We verify that this no longer occurs.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  // We verify that the agent isn't allowed to register.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger the registration attempt.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage);

  // Settle to make sure neither `SlaveRegisteredMessage` nor
  // `ShutdownMessage` are sent.
  Clock::settle();
}


// This test verifies that an agent authorized to register can be
// unauthorized to reregister due to master ACL change (after failover).
TEST_F(MasterAuthorizationTest, UnauthorizedToReregisterAgent)
{
  // Set up ACLs so that the agent can register.
  ACLs acls;
  mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_agents()->set_type(ACL::Entity::ANY);

  master::Flags flags = CreateMasterFlags();
  flags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Previously, agents were shut down when registration failed due to
  // authorization. We verify that this no longer occurs.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Master fails over.
  master->reset();

  // The new master doesn't allow this agent principal to reregister.
  acl->mutable_agents()->set_type(ACL::Entity::NONE);
  flags.acls = acls;

  // The agent should try but not be able to reregister.
  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  EXPECT_NO_FUTURE_PROTOBUFS(SlaveReregisteredMessage(), _, _);

  master = StartMaster(flags);
  ASSERT_SOME(master);

  detector.appoint(master.get()->pid);

  AWAIT_READY(reregisterSlaveMessage);

  // Settle to make sure neither `SlaveReregisteredMessage` nor
  // `ShutdownMessage` are sent.
  Clock::pause();
  Clock::settle();
}


// This test verifies that duplicate agent registration attempts are
// ignored when the ongoing registration is pending in the authorizer.
TEST_F(MasterAuthorizationTest, RetryRegisterAgent)
{
  // Use a paused clock to control agent registration retries.
  Clock::pause();

  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;

  // Expect the authorizer to be called only once, i.e.,
  // the retry is ignored.
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Trigger the first registration attempt (with authentication).
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);

  // Wait until the authorization is in progress.
  AWAIT_READY(authorize);

  // Advance to trigger the second registration attempt.
  Clock::advance(slave::REGISTER_RETRY_INTERVAL_MAX);

  // Settle to make sure the second registration attempt is received
  // by the master. We can verify that it's ignored if the EXPECT_CALL
  // above doesn't oversaturate.
  Clock::settle();

  Future<Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  // Now authorize the agent and verify it's registered.
  promise.set(true);

  AWAIT_READY(slaveRegisteredMessage);
}


// This test verifies that the agent is not allowed to register if it tries to
// statically reserve resources, but it is not allowed to.
TEST_F(MasterAuthorizationTest, UnauthorizedToStaticallyReserveResources)
{
  Clock::pause();

  // Set up ACLs so that the agent can (re)register.
  ACLs acls;

  {
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_agents()->set_type(ACL::Entity::ANY);
  }

  // The agent cannot statically reserve resources of any role.
  {
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->set_type(ACL::Entity::NONE);
  }

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Previously, agents were shut down when registration failed due to
  // authorization. We verify that this no longer occurs.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  // We verify that the agent isn't allowed to register.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus(foo):1;mem(foo):1";

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  // Advance the clock to trigger the registration attempt.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage);

  // Settle to make sure neither `SlaveRegisteredMessage` nor
  // `ShutdownMessage` are sent.
  Clock::settle();
}


// This test verifies that the agent is shut down by the master if it is
// not authorized to statically reserve certain roles.
TEST_F(MasterAuthorizationTest, UnauthorizedToStaticallyReserveRole)
{
  Clock::pause();

  // Set up ACLs so that the agent can (re)register.
  ACLs acls;

  {
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_agents()->set_type(ACL::Entity::ANY);
  }

  // Only `high-security-principal` (not the principal that the agent in
  // this test has) can statically reserve resources for `high-security-role`.
  {
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("high-security-principal");
    acl->mutable_roles()->add_values("high-security-role");
  }

  // No other can statically reserve resources of this role.
  {
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->set_type(ACL::Entity::NONE);
    acl->mutable_roles()->add_values("high-security-role");
  }

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Previously, agents were shut down when registration failed due to
  // authorization. We verify that this no longer occurs.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  // We verify that the agent isn't allowed to register.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus(high-security-role):1;mem(high-security-role):1";

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  // Advance the clock to trigger the registration attempt.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage);

  // Settle to make sure neither `SlaveRegisteredMessage` nor
  // `ShutdownMessage` are sent.
  Clock::settle();
}


// This test verifies that the agent successfully registers while statically
// reserving resource for a certain role when permitted in the ACLs.
TEST_F(MasterAuthorizationTest, AuthorizedToStaticallyReserveRole)
{
  // Set up ACLs so that the agent can register.
  ACLs acls;

  {
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_agents()->set_type(ACL::Entity::ANY);
  }

  // The agent principal is allowed to statically reserve resources of
  // `high-security-role`.
  {
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_roles()->add_values("high-security-role");
  }

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start a slave with no static reservations.
  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus(high-security-role):1;mem(high-security-role):1";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);
}


// This test verifies that the agent is authorized if ACLs don't allow
// any agents to statically reserve resources but the agent only registers
// with unreserved resources.
TEST_F(MasterAuthorizationTest, AuthorizedToRegisterNoStaticReservations)
{
  // Set up ACLs so that the agent can (re)register.
  ACLs acls;

  {
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_agents()->set_type(ACL::Entity::ANY);
  }

  {
    // No agent is allowed to statically reserve resources.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->set_type(ACL::Entity::NONE);
  }

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start a slave with no static reservations.
  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);
}

// This test verifies that when reserving resources the correct
// `RESERVE_RESOURCES` or `UNRESERVE_RESOURCES` authorization
// requests are performed.
TEST_F(MasterAuthorizationTest, ReserveResources)
{
  Clock::pause();

  MockAuthorizer authorizer;

  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  // Reserve `cpus:0.1` for `foo`. This should trigger exactly one authorization
  // request for `RESERVE_RESOURCES`. This is a base case.
  {
    Future<authorization::Request> request;
    EXPECT_CALL(authorizer, authorized(_))
      .WillOnce(DoAll(FutureArg<0>(&request), Return(true)));

    Call call;
    call.set_type(Call::RESERVE_RESOURCES);
    Call::ReserveResources* reserveResources = call.mutable_reserve_resources();

    reserveResources->mutable_agent_id()->set_value(slaveId.value());

    v1::Resource resource = *v1::Resources::parse("cpus", "0.1", "*");
    *reserveResources->add_source() = resource;

    *resource.add_reservations() =
      v1::createDynamicReservationInfo("foo", DEFAULT_CREDENTIAL.principal());
    *reserveResources->add_resources() = resource;

    Future<http::Response> response = http::post(
        master.get()->pid,
        "/api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        stringify(JSON::protobuf(call)),
        stringify(ContentType::JSON));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    AWAIT_READY(request);
    ASSERT_EQ(authorization::Action::RESERVE_RESOURCES, request->action());
    ASSERT_EQ(1, request->object().resource().reservations_size());
    ASSERT_EQ("foo", request->object().resource().reservations(0).role());
  }

  // Reserve `cpus:0.1` for `foo` and refine to `foo/bar`. This should trigger
  // two authorization requests, one for the reservation to `foo` and another
  // one for the reservation refinement to `foo/bar`.
  {
    Future<authorization::Request> request1;
    Future<authorization::Request> request2;
    EXPECT_CALL(authorizer, authorized(_))
      .WillOnce(DoAll(FutureArg<0>(&request1), Return(true)))
      .WillOnce(DoAll(FutureArg<0>(&request2), Return(true)));

    Call call;
    call.set_type(Call::RESERVE_RESOURCES);
    Call::ReserveResources* reserveResources = call.mutable_reserve_resources();

    reserveResources->mutable_agent_id()->set_value(slaveId.value());

    v1::Resource resource = *v1::Resources::parse("cpus", "0.1", "*");
    *reserveResources->add_source() = resource;

    *resource.add_reservations() =
      v1::createDynamicReservationInfo("foo", DEFAULT_CREDENTIAL.principal());
    *resource.add_reservations() = v1::createDynamicReservationInfo(
        "foo/bar", DEFAULT_CREDENTIAL.principal());
    *reserveResources->add_resources() = resource;

    Future<http::Response> response = http::post(
        master.get()->pid,
        "/api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        stringify(JSON::protobuf(call)),
        stringify(ContentType::JSON));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    AWAIT_READY(request1);
    ASSERT_EQ(authorization::Action::RESERVE_RESOURCES, request1->action());
    ASSERT_EQ(1, request1->object().resource().reservations_size());
    ASSERT_EQ("foo", request1->object().resource().reservations(0).role());

    AWAIT_READY(request2);
    ASSERT_EQ(authorization::Action::RESERVE_RESOURCES, request2->action());
    ASSERT_EQ(2, request2->object().resource().reservations_size());
    ASSERT_EQ("foo", request2->object().resource().reservations(0).role());
    ASSERT_EQ("foo/bar", request2->object().resource().reservations(1).role());
  }

  // Re-reserve the `cpus:0.1` reserved above for `foo` and refined to `foo/bar`
  // to `baz` and refined to `baz/bar`. This will trigger two authorization
  // requests to unreserved the resource in the `foo` hierarchy, and two
  // authorization request for the reservations in the `baz` hierarchy.
  {
    Future<authorization::Request> request1;
    Future<authorization::Request> request2;
    Future<authorization::Request> request3;
    Future<authorization::Request> request4;
    EXPECT_CALL(authorizer, authorized(_))
      .WillOnce(DoAll(FutureArg<0>(&request1), Return(true)))
      .WillOnce(DoAll(FutureArg<0>(&request2), Return(true)))
      .WillOnce(DoAll(FutureArg<0>(&request3), Return(true)))
      .WillOnce(DoAll(FutureArg<0>(&request4), Return(true)));

    Call call;
    call.set_type(Call::RESERVE_RESOURCES);
    Call::ReserveResources* reserveResources = call.mutable_reserve_resources();

    reserveResources->mutable_agent_id()->set_value(slaveId.value());

    v1::Resource source = *v1::Resources::parse("cpus", "0.1", "*");
    *source.add_reservations() =
      v1::createDynamicReservationInfo("foo", DEFAULT_CREDENTIAL.principal());
    *source.add_reservations() = v1::createDynamicReservationInfo(
      "foo/bar", DEFAULT_CREDENTIAL.principal());

    v1::Resource resource = *v1::Resources::parse("cpus", "0.1", "*");
    *resource.add_reservations() =
      v1::createDynamicReservationInfo("baz", DEFAULT_CREDENTIAL.principal());
    *resource.add_reservations() = v1::createDynamicReservationInfo(
      "baz/bar", DEFAULT_CREDENTIAL.principal());

    *reserveResources->add_resources() = resource;
    *reserveResources->add_source() = source;

    Future<http::Response> response = http::post(
        master.get()->pid,
        "/api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        stringify(JSON::protobuf(call)),
        stringify(ContentType::JSON));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    AWAIT_READY(request1);
    ASSERT_EQ(authorization::Action::UNRESERVE_RESOURCES, request1->action());
    ASSERT_EQ(2, request1->object().resource().reservations_size());
    ASSERT_EQ("foo", request1->object().resource().reservations(0).role());
    ASSERT_EQ("foo/bar", request1->object().resource().reservations(1).role());

    AWAIT_READY(request2);
    ASSERT_EQ(authorization::Action::UNRESERVE_RESOURCES, request2->action());
    ASSERT_EQ(1, request2->object().resource().reservations_size());
    ASSERT_EQ("foo", request1->object().resource().reservations(0).role());

    AWAIT_READY(request3);
    ASSERT_EQ(authorization::Action::RESERVE_RESOURCES, request3->action());
    ASSERT_EQ(1, request3->object().resource().reservations_size());
    ASSERT_EQ("baz", request3->object().resource().reservations(0).role());

    AWAIT_READY(request4);
    ASSERT_EQ(authorization::Action::RESERVE_RESOURCES, request4->action());
    ASSERT_EQ(2, request4->object().resource().reservations_size());
    ASSERT_EQ("baz", request4->object().resource().reservations(0).role());
    ASSERT_EQ("baz/bar", request4->object().resource().reservations(1).role());
  }
}

class MasterOperationAuthorizationTest
  : public MesosTest,
    public ::testing::WithParamInterface<authorization::Action>
{
public:
  static Resources createAgentResources(const Resources& resources)
  {
    Resources agentResources;
    foreach (
        Resource resource,
        resources - resources.filter(&Resources::hasResourceProvider)) {
      if (Resources::isPersistentVolume(resource)) {
        if (resource.disk().has_source()) {
          resource.mutable_disk()->clear_persistence();
          resource.mutable_disk()->clear_volume();
        } else {
          resource.clear_disk();
        }
      }

      agentResources += resource;
    }

    return agentResources;
  }

  static vector<v1::Offer::Operation> createOperations(
      const v1::FrameworkID& frameworkId,
      const v1::AgentID& agentId,
      const authorization::Action& action)
  {
    switch (action) {
      case authorization::RUN_TASK: {
        const v1::Resources taskResources =
          v1::Resources::parse("cpus:1;mem:32").get();

        v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
            v1::DEFAULT_EXECUTOR_ID,
            None(),
            v1::Resources::parse("cpus:1;mem:32;disk:32").get(),
            v1::ExecutorInfo::DEFAULT,
            frameworkId);

        return {v1::LAUNCH(
                    {v1::createTask(agentId, taskResources, ""),
                     v1::createTask(agentId, taskResources, "")}),
                v1::LAUNCH_GROUP(
                    executorInfo,
                    v1::createTaskGroupInfo(
                        {v1::createTask(agentId, taskResources, ""),
                         v1::createTask(agentId, taskResources, "")}))};
      }
      case authorization::RESERVE_RESOURCES: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resources reserved;
        reserved += v1::createReservedResource(
            "cpus", "1", v1::createDynamicReservationInfo(
                "role", v1::DEFAULT_CREDENTIAL.principal()));
        reserved += v1::createReservedResource(
            "mem", "32", v1::createDynamicReservationInfo(
                "role", v1::DEFAULT_CREDENTIAL.principal()));

        return {v1::RESERVE(reserved, std::move(operationId))};
      }
      case authorization::UNRESERVE_RESOURCES: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resources reserved;
        reserved += v1::createReservedResource(
            "cpus", "1", v1::createDynamicReservationInfo("role"));
        reserved += v1::createReservedResource(
            "mem", "32", v1::createDynamicReservationInfo("role"));

        return {v1::UNRESERVE(reserved, std::move(operationId))};
      }
      case authorization::CREATE_VOLUME: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resources volumes;
        volumes += v1::createPersistentVolume(
            Megabytes(32),
            "role",
            id::UUID::random().toString(),
            "path",
            None(),
            None(),
            v1::DEFAULT_CREDENTIAL.principal());
        volumes += v1::createPersistentVolume(
            Megabytes(32),
            "role",
            id::UUID::random().toString(),
            "path",
            None(),
            None(),
            v1::DEFAULT_CREDENTIAL.principal());

        return {v1::CREATE(volumes, std::move(operationId))};
      }
      case authorization::DESTROY_VOLUME: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resources volumes;
        volumes += v1::createPersistentVolume(
            Megabytes(32), "role", id::UUID::random().toString(), "path");
        volumes += v1::createPersistentVolume(
            Megabytes(32), "role", id::UUID::random().toString(), "path");

        return {v1::DESTROY(volumes, std::move(operationId))};
      }
      case authorization::RESIZE_VOLUME: {
        v1::OperationID operationId1;
        operationId1.set_value(id::UUID::random().toString());

        v1::OperationID operationId2;
        operationId2.set_value(id::UUID::random().toString());

        return {
          v1::GROW_VOLUME(
              v1::createPersistentVolume(
                  Megabytes(32), "role", id::UUID::random().toString(), "path"),
              v1::Resources::parse("disk", "32", "role").get(),
              std::move(operationId1)),
          v1::SHRINK_VOLUME(
              v1::createPersistentVolume(
                  Megabytes(64), "role", id::UUID::random().toString(), "path"),
              mesos::v1::internal::values::parse("32")->scalar(),
              std::move(operationId2))};
      }
      case authorization::CREATE_BLOCK_DISK: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resource raw = v1::createDiskResource(
            "32", "*", None(), None(), v1::createDiskSourceRaw(
                None(), "profile"));
        raw.mutable_provider_id()->set_value("provider");

        return {v1::CREATE_DISK(
            raw,
            v1::Resource::DiskInfo::Source::BLOCK,
            None(),
            std::move(operationId))};
      }
      case authorization::DESTROY_BLOCK_DISK: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resource block = v1::createDiskResource(
            "32", "*", None(), None(), v1::createDiskSourceBlock(
                id::UUID::random().toString(), "profile"));
        block.mutable_provider_id()->set_value("provider");

        return {v1::DESTROY_DISK(block, std::move(operationId))};
      }
      case authorization::CREATE_MOUNT_DISK: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resource raw = v1::createDiskResource(
            "32", "*", None(), None(), v1::createDiskSourceRaw(
                None(), "profile"));
        raw.mutable_provider_id()->set_value("provider");

        return {v1::CREATE_DISK(
            raw,
            v1::Resource::DiskInfo::Source::MOUNT,
            None(),
            std::move(operationId))};
      }
      case authorization::DESTROY_MOUNT_DISK: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resource mount = v1::createDiskResource(
            "32", "*", None(), None(), v1::createDiskSourceMount(
                None(), id::UUID::random().toString(), "profile"));
        mount.mutable_provider_id()->set_value("provider");

        return {v1::DESTROY_DISK(mount, std::move(operationId))};
      }
      case authorization::DESTROY_RAW_DISK: {
        v1::OperationID operationId;
        operationId.set_value(id::UUID::random().toString());

        v1::Resource raw = v1::createDiskResource(
            "32", "*", None(), None(), v1::createDiskSourceRaw(
                id::UUID::random().toString(), "profile"));
        raw.mutable_provider_id()->set_value("provider");

        return {v1::DESTROY_DISK(raw, std::move(operationId))};
      }
      case authorization::UNKNOWN:
      case authorization::REGISTER_FRAMEWORK:
      case authorization::TEARDOWN_FRAMEWORK:
      case authorization::GET_ENDPOINT_WITH_PATH:
      case authorization::VIEW_ROLE:
      case authorization::UPDATE_WEIGHT:
      case authorization::GET_QUOTA:
      case authorization::UPDATE_QUOTA:
      case authorization::UPDATE_QUOTA_WITH_CONFIG:
      case authorization::VIEW_FRAMEWORK:
      case authorization::VIEW_TASK:
      case authorization::VIEW_EXECUTOR:
      case authorization::ACCESS_SANDBOX:
      case authorization::ACCESS_MESOS_LOG:
      case authorization::VIEW_FLAGS:
      case authorization::LAUNCH_NESTED_CONTAINER:
      case authorization::KILL_NESTED_CONTAINER:
      case authorization::WAIT_NESTED_CONTAINER:
      case authorization::LAUNCH_NESTED_CONTAINER_SESSION:
      case authorization::ATTACH_CONTAINER_INPUT:
      case authorization::ATTACH_CONTAINER_OUTPUT:
      case authorization::VIEW_CONTAINER:
      case authorization::SET_LOG_LEVEL:
      case authorization::REMOVE_NESTED_CONTAINER:
      case authorization::REGISTER_AGENT:
      case authorization::UPDATE_MAINTENANCE_SCHEDULE:
      case authorization::GET_MAINTENANCE_SCHEDULE:
      case authorization::START_MAINTENANCE:
      case authorization::STOP_MAINTENANCE:
      case authorization::GET_MAINTENANCE_STATUS:
      case authorization::DRAIN_AGENT:
      case authorization::DEACTIVATE_AGENT:
      case authorization::REACTIVATE_AGENT:
      case authorization::MARK_AGENT_GONE:
      case authorization::LAUNCH_STANDALONE_CONTAINER:
      case authorization::KILL_STANDALONE_CONTAINER:
      case authorization::WAIT_STANDALONE_CONTAINER:
      case authorization::REMOVE_STANDALONE_CONTAINER:
      case authorization::VIEW_STANDALONE_CONTAINER:
      case authorization::MODIFY_RESOURCE_PROVIDER_CONFIG:
      case authorization::MARK_RESOURCE_PROVIDER_GONE:
      case authorization::VIEW_RESOURCE_PROVIDER:
      case authorization::PRUNE_IMAGES:
        return {};
    }

    UNREACHABLE();
  }
};


class ControllableObjectApprover : public ObjectApprover
{
public:
  ControllableObjectApprover(bool permissive_) : permissive(permissive_) {}
  void disable() { permissive.store(false); }

  Try<bool> approved(
      const Option<ObjectApprover::Object>&) const noexcept override
  {
    return permissive.load();
  }

private:
  std::atomic_bool permissive;
};


INSTANTIATE_TEST_CASE_P(
    AllowedAction,
    MasterOperationAuthorizationTest,
    ::testing::Values(
        authorization::RUN_TASK,
        authorization::RESERVE_RESOURCES,
        authorization::UNRESERVE_RESOURCES,
        authorization::CREATE_VOLUME,
        authorization::DESTROY_VOLUME,
        authorization::RESIZE_VOLUME,
        authorization::CREATE_BLOCK_DISK,
        authorization::DESTROY_BLOCK_DISK,
        authorization::CREATE_MOUNT_DISK,
        authorization::DESTROY_MOUNT_DISK),
    [](const testing::TestParamInfo<authorization::Action>& action) {
      return authorization::Action_Name(action.param);
    });


// This test verifies that allowing or denying an action will only result in a
// success or failure on specific operations but not other operations in an
// accept call. This is a regression test for MESOS-9474 and MESOS-9480.
TEST_P(MasterOperationAuthorizationTest, Accept)
{
  Clock::pause();

  const auto controllableApprover =
    std::make_shared<ControllableObjectApprover>(true);

  MockAuthorizer authorizer;

  const authorization::Action allowedAction = GetParam();
  EXPECT_CALL(authorizer, getApprover(_, _))
    .WillRepeatedly(Invoke([controllableApprover, allowedAction](
                               const Option<authorization::Subject>&,
                               const authorization::Action& action) {
      return action == allowedAction ? getAcceptingObjectApprover()
                                     : controllableApprover;
    }));

  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  // First, we create a list of operations to exercise all authorization
  // actions, and compute the total resources needed by these operations and set
  // up their expected terminal states.
  //
  // NOTE: We create some `RUN_TASK` operations in the beginning and some in the
  // end for two reasons: 1. These operations doesn't have operation IDs so
  // needs to be handled differently. 2. By adding some operations for the
  // `RUN_TASK` action in the end, we can verify that their results are not
  // affected by preceding authorizations.
  vector<v1::Offer::Operation> operations;
  v1::Resources totalResources;
  hashmap<v1::TaskID, v1::TaskState> expectedTaskStates;
  hashmap<v1::OperationID, v1::OperationState> expectedOperationStates;

  // NOTE: Because we create operations before getting an offer, we synthesize
  // the framework ID and agent ID here and check them later.
  v1::FrameworkID frameworkId;
  frameworkId.set_value(master.get()->getMasterInfo().id() + "-0000");
  v1::AgentID agentId;
  agentId.set_value(master.get()->getMasterInfo().id() + "-S0");

  auto addRunTaskOperations = [&] {
    foreach (
        v1::Offer::Operation& operation,
        createOperations(frameworkId, agentId, authorization::RUN_TASK)) {
      if (operation.type() == v1::Offer::Operation::LAUNCH) {
        foreach (const v1::TaskInfo& task, operation.launch().task_infos()) {
          totalResources += task.resources();
          totalResources += task.executor().resources();

          expectedTaskStates.put(
              task.task_id(),
              authorization::RUN_TASK == GetParam()
                ? v1::TASK_FINISHED : v1::TASK_ERROR);
        }
      } else if (operation.type() == v1::Offer::Operation::LAUNCH_GROUP) {
        totalResources += operation.launch_group().executor().resources();

        foreach (
            const v1::TaskInfo& task,
            operation.launch_group().task_group().tasks()) {
          totalResources += task.resources();

          expectedTaskStates.put(
              task.task_id(),
              authorization::RUN_TASK == GetParam()
                ? v1::TASK_FINISHED : v1::TASK_ERROR);
        }
      }

      operations.push_back(std::move(operation));
    }
  };

  addRunTaskOperations();

  for (int i = 0; i < authorization::Action_descriptor()->value_count(); i++) {
    const authorization::Action action = static_cast<authorization::Action>(
        authorization::Action_descriptor()->value(i)->number());

    // Skip `RUN_TASK` operations since they are handled separately.
    if (action == authorization::RUN_TASK) {
      continue;
    }

    foreach (
        v1::Offer::Operation& operation,
        createOperations(frameworkId, agentId, action)) {
      Try<Resources> consumed =
        protobuf::getConsumedResources(devolve(operation));
      ASSERT_SOME(consumed);
      totalResources += evolve(consumed.get());

      ASSERT_TRUE(operation.has_id());
      expectedOperationStates.put(
          operation.id(),
          action == GetParam() ? v1::OPERATION_FINISHED : v1::OPERATION_ERROR);

      operations.push_back(std::move(operation));
    }
  }

  addRunTaskOperations();

  // Then, we register a mock agent that has sufficient resources to exercise
  // all operations and simply replies `TASK_FINISHED` or `OPERATION_FINISHED`
  // for all tasks or operations it receives.
  //
  // NOTE: Since dynamic reservations, persistent volumes and resource
  // provider resources cannot be specified through the `--resources` flag, we
  // intercept the `RegisterSlaveMessage` and `UpdateSlaveMessage` to inject
  // resources required by all operations then forward them to the master.
  Future<RegisterSlaveMessage> registerSlaveMessage =
    DROP_PROTOBUF(RegisterSlaveMessage(), _, _);
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlaveMessage =
    DROP_PROTOBUF(UpdateSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);
  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  EXPECT_CALL(*slave.get()->mock(), runTask(_, _, _, _, _, _, _))
    .WillRepeatedly(Invoke([&](
        const process::UPID& from,
        const FrameworkInfo& frameworkInfo,
        const FrameworkID& frameworkId,
        const process::UPID& pid,
        const TaskInfo& task,
        const std::vector<ResourceVersionUUID>& resourceVersionUuids,
        const Option<bool>& launchExecutor) {
      ASSERT_TRUE(frameworkInfo.has_id());

      StatusUpdateMessage message;
      message.set_pid(slave.get()->pid);
      *message.mutable_update() = protobuf::createStatusUpdate(
          frameworkInfo.id(),
          devolve(agentId),
          task.task_id(),
          TASK_FINISHED,
          TaskStatus::SOURCE_SLAVE,
          id::UUID::random());

      process::post(slave.get()->pid, master.get()->pid, message);
    }));

  EXPECT_CALL(*slave.get()->mock(), runTaskGroup(_, _, _, _, _, _))
    .WillRepeatedly(Invoke([&](
        const process::UPID& from,
        const FrameworkInfo& frameworkInfo,
        const ExecutorInfo& executorInfo,
        const TaskGroupInfo& taskGroup,
        const std::vector<ResourceVersionUUID>& resourceVersionUuids,
        const Option<bool>& launchExecutor) {
      ASSERT_TRUE(frameworkInfo.has_id());

      foreach (const TaskInfo& task, taskGroup.tasks()) {
        StatusUpdateMessage message;
        message.set_pid(slave.get()->pid);
        *message.mutable_update() = protobuf::createStatusUpdate(
            frameworkInfo.id(),
            devolve(agentId),
            task.task_id(),
            TASK_FINISHED,
            TaskStatus::SOURCE_SLAVE,
            id::UUID::random());

        process::post(slave.get()->pid, master.get()->pid, message);
      }
    }));

  EXPECT_CALL(*slave.get()->mock(), applyOperation(_))
    .WillRepeatedly(Invoke([&](const ApplyOperationMessage& message) {
      ASSERT_TRUE(message.has_framework_id());
      ASSERT_TRUE(message.operation_info().has_id());

      process::post(
          slave.get()->pid,
          master.get()->pid,
          protobuf::createUpdateOperationStatusMessage(
              message.operation_uuid(),
              protobuf::createOperationStatus(
                  OPERATION_FINISHED,
                  message.operation_info().id(),
                  None(),
                  None(),
                  None(),
                  devolve(agentId)),
              None(),
              message.framework_id(),
              devolve(agentId)));
    }));

  slave.get()->start();

  // Settle the clock to ensure that the master has been detected by the agent,
  // then advance the clock to trigger an authentication and a registration.
  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(registerSlaveMessage);

  {
    RegisterSlaveMessage message = registerSlaveMessage.get();
    *message.mutable_slave()->mutable_resources() =
      createAgentResources(devolve(totalResources));
    *message.mutable_checkpointed_resources() =
      devolve(totalResources).filter(needCheckpointing);

    process::post(slave.get()->pid, master.get()->pid, message);
  }

  AWAIT_READY(slaveRegisteredMessage);
  EXPECT_EQ(devolve(agentId), slaveRegisteredMessage->slave_id());

  AWAIT_READY(updateSlaveMessage);

  {
    UpdateSlaveMessage message = updateSlaveMessage.get();
    UpdateSlaveMessage::ResourceProvider* resourceProvider =
      message.mutable_resource_providers()->add_providers();
    resourceProvider->mutable_info()->mutable_id()->set_value("provider");
    resourceProvider->mutable_info()->set_type("resource_provider_type");
    resourceProvider->mutable_info()->set_name("resource_provider_name");
    *resourceProvider->mutable_total_resources() =
      devolve(totalResources).filter(&Resources::hasResourceProvider);
    resourceProvider->mutable_operations();
    resourceProvider->mutable_resource_version_uuid()->set_value(
        id::UUID::random().toBytes());

    process::post(slave.get()->pid, master.get()->pid, message);
  }

  // Settle the clock to ensure that the resource providers has been updated.
  Clock::settle();

  // Finally, we register a framework to exercise all operations, and check that
  // only authorized tasks and operations are finished.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _)).WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  EXPECT_EQ(frameworkId, subscribed->framework_id());

  // Start to deny disallowed actions.
  controllableApprover->disable();

  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  hashmap<v1::TaskID, Future<v1::scheduler::Event::Update>>
    actualTaskStatusUpdates;
  hashmap<v1::OperationID, Future<v1::scheduler::Event::UpdateOperationStatus>>
    actualOperationStatusUpdates;

  foreachkey (const v1::TaskID& taskId, expectedTaskStates) {
    EXPECT_CALL(*scheduler, update(_, AllOf(
        TaskStatusUpdateTaskIdEq(taskId),
        Truly([](const v1::scheduler::Event::Update& update) {
          return protobuf::isTerminalState(devolve(update.status()).state());
        }))))
      .WillOnce(FutureArg<1>(&actualTaskStatusUpdates[taskId]));
  }

  foreachkey (const v1::OperationID& operationId, expectedOperationStates) {
    EXPECT_CALL(*scheduler, updateOperationStatus(_, AllOf(
        v1::scheduler::OperationStatusUpdateOperationIdEq(operationId),
        Truly([](const v1::scheduler::Event::UpdateOperationStatus& update) {
          return protobuf::isTerminalState(devolve(update.status()).state());
        }))))
      .WillOnce(FutureArg<1>(&actualOperationStatusUpdates[operationId]));
  }

  mesos.send(v1::createCallAccept(
      subscribed->framework_id(),
      offers->offers(0),
      operations));

  foreachpair (
      const v1::TaskID& taskId,
      const Future<v1::scheduler::Event::Update>& update,
      actualTaskStatusUpdates) {
    AWAIT_READY(update);
    EXPECT_EQ(expectedTaskStates[taskId], update->status().state());
  }

  foreachpair (
      const v1::OperationID& operationId,
      const Future<v1::scheduler::Event::UpdateOperationStatus>& update,
      actualOperationStatusUpdates) {
    AWAIT_READY(update);
    EXPECT_EQ(expectedOperationStates[operationId], update->status().state());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
