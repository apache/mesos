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

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace tests {

using std::string;


template <typename T>
class AuthorizationTest : public MesosTest {};


typedef ::testing::Types<
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    tests::Module<Authorizer, TestLocalAuthorizer>,
#endif // __WINDOWS__
    LocalAuthorizer> AuthorizerTypes;


TYPED_TEST_CASE(AuthorizationTest, AuthorizerTypes);


TYPED_TEST(AuthorizationTest, AnyPrincipalRunAsUser)
{
  ACLs acls;

  {
    // Any principal can run as "guest" user.
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->add_values("guest");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can run as "guest", using TaskInfo.command.user,
  // TaskInfo.ExecutorInfo.command.user, or FrameworkInfo.user.
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    CommandInfo* commandInfo = taskInfo.mutable_command();
    commandInfo->set_user("guest");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("guest");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    taskInfo.mutable_executor()->mutable_command()->set_user("guest");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "foo" can run as "root" since the ACLs are permissive.
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    CommandInfo* commandInfo = taskInfo.mutable_command();
    commandInfo->set_user("root");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "foo" can run as "root" since the ACLs are permissive.
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("root");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    taskInfo.mutable_executor()->mutable_command()->set_user("root");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, NoPrincipalRunAsUser)
{
  // No principal can run as "root" user.
  ACLs acls;
  {
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
    acl->mutable_users()->add_values("root");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" cannot run as "root".
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    CommandInfo* commandInfo = taskInfo.mutable_command();
    commandInfo->set_user("root");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("root");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    taskInfo.mutable_executor()->mutable_command()->set_user("root");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, PrincipalRunAsAnyUser)
{
  // A principal "foo" can run as any user.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.

  {
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can run as "user1" and "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, AnyPrincipalRunAsAnyUser)
{
  // Any principal can run as any user.
  ACLs acls;
  acls.set_permissive(false); // Restrictive.

  {
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principals "foo" and "bar" can run as "user1" and "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, OnlySomePrincipalsRunAsSomeUsers)
{
  // Only some principals can run as some users.
  ACLs acls;

  {
    // ACL for some principals to run as some users.
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("user1");
    acl->mutable_users()->add_values("user2");
  }

  {
    // ACL for no one else to run as some users.
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
    acl->mutable_users()->add_values("user1");
    acl->mutable_users()->add_values("user2");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principals "foo" and "bar" can run as "user1" and "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "baz" cannot run as "user1".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("baz");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot run as "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("baz");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, SomePrincipalOnlySomeUser)
{
  // Some principal can run as only some user.
  ACLs acls;

  // ACL for some principal to run as some user.
  {
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->add_values("user1");
  }

  {
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can run as "user1".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "foo" cannot run as "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" can run as "user1" and "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, PrincipalRunAsSomeUserRestrictive)
{
  ACLs acls;
  acls.set_permissive(false); // Restrictive.

  {
    // A principal can run as "user1";
    mesos::ACL::RunTask* acl = acls.add_run_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->add_values("user1");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can run as "user1".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user1");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "foo" cannot run as "user2".
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    taskInfo.mutable_command()->set_user("user2");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("foo");

    TaskInfo taskInfo;
    taskInfo.mutable_executor()->mutable_command()->set_user("user2");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot run as "user2" since no ACL is set.
  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    TaskInfo taskInfo;
    CommandInfo* commandInfo = taskInfo.mutable_command();
    commandInfo->set_user("user2");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    FrameworkInfo frameworkInfo;
    frameworkInfo.set_user("user2");

    request.mutable_object()->mutable_framework_info()->CopyFrom(frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;

    request.set_action(authorization::RUN_TASK);
    request.mutable_subject()->set_value("bar");

    TaskInfo taskInfo;
    taskInfo.mutable_executor()->mutable_command()->set_user("user2");

    request.mutable_object()->mutable_task_info()->CopyFrom(taskInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, AnyPrincipalOfferedRole)
{
  ACLs acls;

  {
    // Any principal can be offered "*" role's resources.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values("*");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principals "foo" and "bar" can be offered "*" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("*");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->set_role("*");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("*");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->set_role("*");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, SomePrincipalsOfferedRole)
{
  ACLs acls;

  {
    // Some principals can be offered "ads" role's resources.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("ads");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principals "foo", "bar" and "baz" (no ACL) can be offered "ads"
  // role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->set_role("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->set_role("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->mutable_framework_info()->set_role("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, PrincipalOfferedRole)
{
  // Only a principal can be offered "analytics" role's resources.
  ACLs acls;

  {
    // ACL for a principal to be offered "analytics" role's resources.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->add_values("analytics");
  }

  {
    // ACL for no one else to be offered "analytics" role's resources.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
    acl->mutable_roles()->add_values("analytics");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->set_role("analytics");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->set_role("analytics");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, PrincipalNotOfferedAnyRoleRestrictive)
{
  ACLs acls;
  acls.set_permissive(false);

  {
    // A principal "foo" can be offered "analytics" role's resources.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->add_values("analytics");
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->set_role("analytics");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->set_role("analytics");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot be offered "ads" role's resources because no ACL.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->set_role("ads");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// Checks that the behavior of the authorizer is correct when using
// hierarchical roles while registering frameworks.
TYPED_TEST(AuthorizationTest, RegisterFrameworkHierarchical)
{
  ACLs acls;

  {
    // "elizabeth-ii" principal can register frameworks with role
    // `king` and its nested ones.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // "charles" principal can register frameworks with just roles
    // nested under `king`.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // "j-welby" principal register frameworks with just role 'king'.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can register frameworks in any role.
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to register frameworks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->mutable_framework_info()->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
        ->mutable_framework_info()->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
        ->mutable_framework_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->mutable_framework_info()->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->mutable_framework_info()->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
        ->mutable_framework_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->mutable_framework_info()->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
        ->mutable_framework_info()->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
        ->mutable_framework_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// Tests the authorization of ACLs used for dynamic reservation of resources.
TYPED_TEST(AuthorizationTest, Reserve)
{
  ACLs acls;

  {
    // Principals "foo" and "bar" can reserve resources for any role.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "baz" can only reserve resources for the "ads" role.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->add_values("ads");
  }

  {
    // Principal "elizabeth-ii" can reserve for the "king" role and its
    // nested ones.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can reserve for any role below the "king/" role.
    // Not in "king" itself.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can reserve only for the "king" role but not
    // in any nested one.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can reserve resources.
    mesos::ACL::ReserveResources* acl = acls.add_reserve_resources();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principals "foo" and "bar" can reserve resources for any role,
  // so requests 1 and 2 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "baz" can only reserve resources for the "ads" role, so request 3
  // will pass, but requests 4 and 5 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("ads");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("baz");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("ads");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to reserve resources in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of ACLs used for unreserve
// operations on dynamically reserved resources.
TYPED_TEST(AuthorizationTest, Unreserve)
{
  ACLs acls;

  {
    // "foo" principal can unreserve its own resources.
    mesos::ACL::UnreserveResources* acl = acls.add_unreserve_resources();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_reserver_principals()->add_values("foo");
  }

  {
    // "bar" principal cannot unreserve anyone's resources.
    mesos::ACL::UnreserveResources* acl = acls.add_unreserve_resources();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "ops" principal can unreserve anyone's resources.
    mesos::ACL::UnreserveResources* acl = acls.add_unreserve_resources();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_reserver_principals()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No other principals can unreserve resources.
    mesos::ACL::UnreserveResources* acl = acls.add_unreserve_resources();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can unreserve its own resources.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot unreserve anyone's
  // resources, so requests 2 and 3 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can unreserve anyone's resources,
  // so requests 4 and 5 will succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("ops");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("ops");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_principal("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// Tests the authorization of ACLs used for the creation of persistent volumes.
TYPED_TEST(AuthorizationTest, CreateVolume)
{
  ACLs acls;

  {
    // Principal "foo" can create volumes for any role.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only create volumes for the "panda" role.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot create volumes.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can create volumes for the "king" role and its
    // nested ones.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can create volumes for any role below the "king/"
    // role. Not in "king" itself.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can create volumes only for the "king" role but
    // not in any nested one.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can create volumes.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // No other principals can create volumes.
    mesos::ACL::CreateVolume* acl = acls.add_create_volumes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can create volumes for any role, so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can create volumes for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot create volumes for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot create volumes for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to create volumes in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of ACLs used for destruction
// operations on persistent volumes.
TYPED_TEST(AuthorizationTest, DestroyVolume)
{
  ACLs acls;

  {
    // "foo" principal can destroy its own volumes.
    mesos::ACL::DestroyVolume* acl = acls.add_destroy_volumes();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_creator_principals()->add_values("foo");
  }

  {
    // "bar" principal cannot destroy anyone's volumes.
    mesos::ACL::DestroyVolume* acl = acls.add_destroy_volumes();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "ops" principal can destroy anyone's volumes.
    mesos::ACL::DestroyVolume* acl = acls.add_destroy_volumes();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_creator_principals()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No other principals can destroy volumes.
    mesos::ACL::DestroyVolume* acl = acls.add_destroy_volumes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can destroy its own volumes, so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot destroy anyone's
  // volumes, so requests 2 and 3 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can destroy anyone's volumes,
  // so requests 4 and 5 will succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("foo");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("ops");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("ops");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("bar");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->mutable_resource()->mutable_disk()
        ->mutable_persistence()->set_principal("foo");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// Tests the authorization of ACLs used for resizing volumes.
TYPED_TEST(AuthorizationTest, ResizeVolume)
{
  ACLs acls;

  {
    // Principal "foo" can resize volumes for any role.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only resize volumes for the "panda" role.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot resize volumes.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can resize volumes for the "king" role and its
    // nested ones.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can resize volumes for any role below the "king/"
    // role. Not in "king" itself.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can resize volumes only for the "king" role but
    // not in any nested one.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can resize volumes.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // No other principals can resize volumes.
    mesos::ACL::ResizeVolume* acl = acls.add_resize_volumes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can create volumes for any role, so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can create volumes for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot resize volumes for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot resize volumes for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // "elizabeth-ii" has full permissions for the "king" role as well as all
  // its nested roles. She should be able to resize volumes in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // "charles" doesn't have permissions for the "king" role, so the first
  // test should fail. However he has permissions for "king"'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // "j-welby" only has permissions for the role "king" itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESIZE_VOLUME);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to update quotas.
TYPED_TEST(AuthorizationTest, UpdateQuota)
{
  ACLs acls;

  {
    // "foo" principal can update quotas for all roles.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can update quotas for "dev" role.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("dev");
  }

  {
    // Anyone can update quotas for "test" role.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values("test");
  }

  {
    // Principal "elizabeth-ii" can update quotas for the "king" role and its
    // nested ones.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can update quotas for any role below the "king/"
    // role. Not in "king" itself.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can update quotas only for the "king" role but
    // not in any nested one.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can update quotas.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can update quota for all roles, so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_quota_info()->set_role("prod");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can update quotas for role "dev", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_quota_info()->set_role("dev");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can only update quotas for role "dev", so this will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_quota_info()->set_role("prod");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Anyone can update quotas for role "test", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_object()->mutable_quota_info()->set_role("test");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("jeff");
    request.mutable_object()->mutable_quota_info()->set_role("prod");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to update quotas in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->mutable_quota_info()->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->mutable_quota_info()->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
        ->mutable_quota_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->mutable_quota_info()->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->mutable_quota_info()->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
        ->mutable_quota_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->mutable_quota_info()->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->mutable_quota_info()->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
        ->mutable_quota_info()->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to update quota configs.
TYPED_TEST(AuthorizationTest, UpdateQuotaConfig)
{
  ACLs acls;

  {
    // "foo" principal can update quota configs for all roles.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can update quota configs for "dev" role.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("dev");
  }

  {
    // Anyone can update quota configs for "test" role.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values("test");
  }

  {
    // Principal "elizabeth-ii" can update quota configs for the "king" role
    // and its nested ones.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can update quota configs for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can update quota configs only for the "king" role
    // but not in any nested one.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can update quota configs.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can update quota configs for all roles, so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can update quota configs for role "dev", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("dev");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can only update quota configs for role "dev",
  // so this will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Anyone can update quota configs for role "test", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_object()->set_value("test");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("jeff");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to update quota configs in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
        ->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
        ->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_CONFIG);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
        ->set_value("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to ViewFramework.
TYPED_TEST(AuthorizationTest, ViewFramework)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view no frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can see frameworks running under user "bar".
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can see all frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can view any frameworks.
    mesos::ACL::ViewFramework* acl = acls.add_view_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Create FrameworkInfo with a generic user as object to be authorized.
  FrameworkInfo frameworkInfo;
  {
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("f");
  }

  // Create FrameworkInfo with user "bar" as object to be authorized.
  FrameworkInfo frameworkInfoBar;
  {
    frameworkInfoBar.set_user("bar");
    frameworkInfoBar.set_name("f");
  }

  // Principal "foo" cannot view frameworkInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FRAMEWORK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view a framework Info running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can view a frameworkInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FRAMEWORK);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a frameworkInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FRAMEWORK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to ViewContainer.
TYPED_TEST(AuthorizationTest, ViewContainer)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view no containers.
    mesos::ACL::ViewContainer* acl = acls.add_view_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can see containers running under user "bar".
    mesos::ACL::ViewContainer* acl = acls.add_view_containers();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can see all containers.
    mesos::ACL::ViewContainer* acl = acls.add_view_containers();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can view any containers.
    mesos::ACL::ViewContainer* acl = acls.add_view_containers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Create FrameworkInfo with a generic user as object to be authorized.
  FrameworkInfo frameworkInfo;
  {
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("f");
  }

  // Create FrameworkInfo with user "bar" as object to be authorized.
  FrameworkInfo frameworkInfoBar;
  {
    frameworkInfoBar.set_user("bar");
    frameworkInfoBar.set_name("f");
  }

  // Principal "foo" cannot view containers running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view containers running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can view containers running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view containers running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to ViewTasks.
TYPED_TEST(AuthorizationTest, ViewTask)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view no Task.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can see tasks running under user "bar".
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can see all tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can view any tasks.
    mesos::ACL::ViewTask* acl = acls.add_view_tasks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Create TaskInfo with a generic user as object to be authorized.
  TaskInfo taskInfo;
  {
    taskInfo.set_name("Task");
    taskInfo.mutable_task_id()->set_value("t");
    taskInfo.mutable_slave_id()->set_value("s");
    taskInfo.mutable_command()->set_value("echo hello");
    taskInfo.mutable_command()->set_user("user");
  }

  // Create TaskInfo with user "bar" as object to be authorized.
  TaskInfo taskInfoBar;
  {
    taskInfoBar.set_name("Task");
    taskInfoBar.mutable_task_id()->set_value("t");
    taskInfoBar.mutable_slave_id()->set_value("s");
    taskInfoBar.mutable_command()->set_value("echo hello");
    taskInfoBar.mutable_command()->set_user("bar");
  }

  // Create TaskInfo with user "bar" as object to be authorized.
  TaskInfo taskInfoNoUser;
  {
    taskInfoNoUser.set_name("Task");
    taskInfoNoUser.mutable_task_id()->set_value("t");
    taskInfoNoUser.mutable_slave_id()->set_value("s");
    taskInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Create ExecutorInfo with a generic user in command.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Task");
    executorInfoBar.mutable_executor_id()->set_value("t");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create TaskInfo with ExecutorInfo containing generic user.
  TaskInfo taskInfoExecutor;
  {
    taskInfoExecutor.set_name("Task");
    taskInfoExecutor.mutable_task_id()->set_value("t");
    taskInfoExecutor.mutable_slave_id()->set_value("s");
    taskInfoExecutor.mutable_command()->set_value("echo hello");
    taskInfoExecutor.mutable_executor()->MergeFrom(executorInfo);
  }

  // Create TaskInfo with ExecutorInfo containing user "bar".
  TaskInfo taskInfoExecutorBar;
  {
    taskInfoExecutorBar.set_name("Task");
    taskInfoExecutorBar.mutable_task_id()->set_value("t");
    taskInfoExecutorBar.mutable_slave_id()->set_value("s");
    taskInfoExecutorBar.mutable_executor()->MergeFrom(executorInfoBar);
  }

  // Create Task with a generic user as object to be authorized.
  Task task;
  {
    task.set_name("Task");
    task.mutable_task_id()->set_value("t");
    task.mutable_slave_id()->set_value("s");
    task.set_state(TaskState::TASK_STARTING);
    task.set_user("user");
  }

  // Create Task with user "bar" as object to be authorized.
  Task taskBar;
  {
    taskBar.set_name("Task");
    taskBar.mutable_task_id()->set_value("t");
    taskBar.mutable_slave_id()->set_value("s");
    taskBar.set_state(TaskState::TASK_STARTING);
    taskBar.set_user("bar");
  }

  // Create FrameworkInfo with a generic user as object to be authorized.
  FrameworkInfo frameworkInfo;
  {
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("f");
  }

  // Create FrameworkInfo with user "bar" as object to be authorized.
  FrameworkInfo frameworkInfoBar;
  {
    frameworkInfoBar.set_user("bar");
    frameworkInfoBar.set_name("f");
  }

  // Checks for the combination TaskInfo and FrameworkInfo.

  // Principal "foo" cannot view a request with taskInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view a request with taskInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can view a request with taskInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with taskInfo and frameworkInfo
  // running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with a taskInfo without user
  // and frameworkInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view a request with a taskInfo containing an
  // executorInfo with generic user.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task_info()->MergeFrom(taskInfoExecutor);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with a taskInfo containing an
  // executorInfo with user bar.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task_info()->MergeFrom(
        taskInfoExecutorBar);

    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Checks for the combination Task and FrameworkInfo.

  // Principal "foo" cannot view a request with task and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_task()->MergeFrom(task);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view a request with task and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task()->MergeFrom(task);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can view a request with taskInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_task()->MergeFrom(task);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with task and frameworkInfo
  // running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_TASK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_task()->MergeFrom(taskBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to ViewExecutor.
TYPED_TEST(AuthorizationTest, ViewExecutor)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view no executor.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can see executors running under user "bar".
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can see all executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can view any executors.
    mesos::ACL::ViewExecutor* acl = acls.add_view_executors();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Create ExecutorInfo with a generic user in command as object to
  // be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Create FrameworkInfo with a generic user as object to be authorized.
  FrameworkInfo frameworkInfo;
  {
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("f");
  }

  // Create FrameworkInfo with user "bar" as object to be authorized.
  FrameworkInfo frameworkInfoBar;
  {
    frameworkInfoBar.set_user("bar");
    frameworkInfoBar.set_name("f");
  }

  // Principal "foo" cannot view a request with executorInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_EXECUTOR);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot view a request with executorInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_EXECUTOR);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can view a request with executorInfo and frameworkInfo
  // running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_EXECUTOR);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
      frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with executorInfo and frameworkInfo
  // running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_EXECUTOR);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);

    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can view a request with a executorInfo without user
  // and frameworkInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_EXECUTOR);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);

    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of sandbox access.
TYPED_TEST(AuthorizationTest, SandBoxAccess)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot view any sandboxes.
    mesos::ACL::AccessSandbox* acl = acls.add_access_sandboxes();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can see sandboxes running under user "bar".
    mesos::ACL::AccessSandbox* acl = acls.add_access_sandboxes();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can see all sandboxes.
    mesos::ACL::AccessSandbox* acl = acls.add_access_sandboxes();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can view any sandboxes.
    mesos::ACL::AccessSandbox* acl = acls.add_access_sandboxes();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Create FrameworkInfo with a generic user as object to be authorized.
  FrameworkInfo frameworkInfo;
  {
    frameworkInfo.set_user("user");
    frameworkInfo.set_name("f");
  }

  // Create FrameworkInfo with user "bar" as object to be authorized.
  FrameworkInfo frameworkInfoBar;
  {
    frameworkInfoBar.set_user("bar");
    frameworkInfoBar.set_name("f");
  }

  // Principal "foo" cannot access a sandbox with a request with
  // ExecutorInfo and FrameworkInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_SANDBOX);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot access a sandbox with a request with
  // ExecutorInfo and FrameworkInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_SANDBOX);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can access a sandbox with a request with
  // ExecutorInfo and FrameworkInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_SANDBOX);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
      frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can access a sandbox with a request with
  // ExecutorInfo and FrameworkInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_SANDBOX);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);

    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can access a sandbox with a request with a
  // ExecutorInfo without user and FrameworkInfo running with user
  // "bar".
  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_SANDBOX);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);

    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of launching sessions in nested containers.
TYPED_TEST(AuthorizationTest, LaunchNestedContainerSessions)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "ops" can launch sessions in any parent container.
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
        acls.add_launch_nested_container_sessions_under_parent_with_user();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "foo" cannot launch sessions in any container running as any user.
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
        acls.add_launch_nested_container_sessions_under_parent_with_user();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" can launch sessions nested under a container running as
    // linux user "bar".
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
        acls.add_launch_nested_container_sessions_under_parent_with_user();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // No one else can launch sessions in nested containers.
    mesos::ACL::LaunchNestedContainerSessionUnderParentWithUser* acl =
        acls.add_launch_nested_container_sessions_under_parent_with_user();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "foo" principal cannot launch sessions as commands in any parent
    // container. They may still get to launch container sessions if they
    // are allowed to launch nested container sessions whose executors are
    // running as a given user for which "foo" has permissions and the
    // session uses a `container_info` instead of a `command_info`.
    mesos::ACL::LaunchNestedContainerSessionAsUser* acl =
        acls.add_launch_nested_container_sessions_as_user();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can launch sessions running as user "bar".
    mesos::ACL::LaunchNestedContainerSessionAsUser* acl =
        acls.add_launch_nested_container_sessions_as_user();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can launch sessions as any linux user.
    mesos::ACL::LaunchNestedContainerSessionAsUser* acl =
        acls.add_launch_nested_container_sessions_as_user();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can launch sessions as any user.
    mesos::ACL::LaunchNestedContainerSessionAsUser* acl =
        acls.add_launch_nested_container_sessions_as_user();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("s");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoCommand;
  {
    executorInfoNoCommand.set_name("Executor");
    executorInfoNoCommand.mutable_executor_id()->set_value("t");
  }

  // Principal "foo" cannot launch a session with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot launch a session with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a session with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a session with a request with the
  // default `FrameworkInfo.user`.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a session with a request where the
  // executor is running as a container, it falls back to the
  // `FrameworkInfo.user`.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoCommand);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can launch a session as user "bar" under parent containers
  // running as user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Create CommandInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  CommandInfo commandInfo;
  {
    commandInfo.set_value("echo hello");
    commandInfo.set_user("user");
  }

  // Create CommandInfo with user "bar" in command as object to
  // be authorized.
  CommandInfo commandInfoBar;
  {
    commandInfoBar.set_value("echo hello");
    commandInfoBar.set_user("bar");
  }

  // Create CommandInfo with no user in command as object to
  // be authorized.
  CommandInfo commandInfoNoUser;
  {
    commandInfoNoUser.set_value("echo hello");
  }

  // Principal "foo" cannot launch a session with a request with
  // ExecutorInfo and CommandInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot launch a session with a request with
  // CommandInfo running with user "user", even if the ExecutorInfo
  // is running as "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot launch a session with a request with
  // CommandInfo and ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a sessions with any combination of requests.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER_SESSION);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoNoUser);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoNoUser);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of attaching to the input stream of a container.
TYPED_TEST(AuthorizationTest, AttachContainerInput)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot attach to the input of any container.
    mesos::ACL::AttachContainerInput* acl =
        acls.add_attach_containers_input();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can attach to the input of containers running under
    // user "bar".
    mesos::ACL::AttachContainerInput* acl =
        acls.add_attach_containers_input();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can attach to the input of all containers.
    mesos::ACL::AttachContainerInput* acl =
        acls.add_attach_containers_input();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can attach to the input of any container.
    mesos::ACL::AttachContainerInput* acl =
        acls.add_attach_containers_input();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Principal "foo" cannot attach to the input of a container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_INPUT);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot attach to the input of a container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_INPUT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can attach to the input of a container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_INPUT);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can attach to the input of a container with a request with
  // ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_INPUT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can attach to the input of a container with a request with
  // an ExecutorInfo without user.
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_INPUT);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of attaching to the output stream of a
// container.
TYPED_TEST(AuthorizationTest, AttachContainerOutput)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot attach to the output of any container.
    mesos::ACL::AttachContainerOutput* acl =
        acls.add_attach_containers_output();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can attach to the output of containers running under
    // user "bar".
    mesos::ACL::AttachContainerOutput* acl =
        acls.add_attach_containers_output();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can attach to the output of all containers.
    mesos::ACL::AttachContainerOutput* acl =
        acls.add_attach_containers_output();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can attach to the output of any container.
    mesos::ACL::AttachContainerOutput* acl =
        acls.add_attach_containers_output();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Principal "foo" cannot attach to the output of a container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_OUTPUT);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot attach to the output of a container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_OUTPUT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can attach to the output of a container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_OUTPUT);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can attach to the output of a container with a request with
  // ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_OUTPUT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can attach to the output of a container with a request with
  // an ExecutorInfo without user.
  {
    authorization::Request request;
    request.set_action(authorization::ATTACH_CONTAINER_OUTPUT);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of launching nested containers.
TYPED_TEST(AuthorizationTest, LaunchNestedContainers)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "ops" can launch nested containers whose executor runs as any user.
    mesos::ACL::LaunchNestedContainerUnderParentWithUser* acl =
        acls.add_launch_nested_containers_under_parent_with_user();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "foo" cannot launch nested containers.
    mesos::ACL::LaunchNestedContainerUnderParentWithUser* acl =
        acls.add_launch_nested_containers_under_parent_with_user();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" can launch nested containers under executors running as
    // linux user "bar".
    mesos::ACL::LaunchNestedContainerUnderParentWithUser* acl =
        acls.add_launch_nested_containers_under_parent_with_user();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // No one else can launch nested containers.
    mesos::ACL::LaunchNestedContainerUnderParentWithUser* acl =
        acls.add_launch_nested_containers_under_parent_with_user();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "foo" principal cannot launch nested containers as commands
    // under any parent container. They may still get to launch nested
    // container if they are allowed to do so for executors which run as
    // a given user for which "foo" has permissions and the session
    // uses a `container_info` instead of a `command_info`.
    mesos::ACL::LaunchNestedContainerAsUser* acl =
        acls.add_launch_nested_containers_as_user();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can launch nested containers as user "bar".
    mesos::ACL::LaunchNestedContainerAsUser* acl =
        acls.add_launch_nested_containers_as_user();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can launch nested containers any linux user.
    mesos::ACL::LaunchNestedContainerAsUser* acl =
        acls.add_launch_nested_containers_as_user();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can launch nested containers as any user.
    mesos::ACL::LaunchNestedContainerAsUser* acl =
        acls.add_launch_nested_containers_as_user();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("s");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Create ExecutorInfo with no command as object to be authorized.
  ExecutorInfo executorInfoNoCommand;
  {
    executorInfoNoCommand.set_name("Executor");
    executorInfoNoCommand.mutable_executor_id()->set_value("t");
  }

  // Principal "foo" cannot launch a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot launch a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a nested container with a request with the
  // default `FrameworkInfo.user`.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch a nested container with a request
  // where the executor is running as a container, it falls back
  // to the `FrameworkInfo.user`.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoCommand);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can launch a nested container as user "bar" under parent
  // containers running as user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Create CommandInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  CommandInfo commandInfo;
  {
    commandInfo.set_value("echo hello");
    commandInfo.set_user("user");
  }

  // Create CommandInfo with user "bar" in command as object to
  // be authorized.
  CommandInfo commandInfoBar;
  {
    commandInfoBar.set_value("echo hello");
    commandInfoBar.set_user("bar");
  }

  // Create CommandInfo with no user in command as object to
  // be authorized.
  CommandInfo commandInfoNoUser;
  {
    commandInfoNoUser.set_value("echo hello");
  }

  // Principal "foo" cannot launch a session with a request with
  // ExecutorInfo and CommandInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot launch a nested container with a request with
  // CommandInfo running with user "user", even if the ExecutorInfo
  // is running as "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" can launch a nested container with a request with
  // CommandInfo and ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can launch nested containers with any combination of
  // requests.
  {
    authorization::Request request;
    request.set_action(authorization::LAUNCH_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_command_info()->MergeFrom(commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoNoUser);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfo);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoBar);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));

    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_command_info()->MergeFrom(
        commandInfoNoUser);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of waiting for a nested container.
TYPED_TEST(AuthorizationTest, WaitNestedContainer)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot wait of any nested container.
    mesos::ACL::WaitNestedContainer* acl =
        acls.add_wait_nested_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can wait for nested containers running under
    // user "bar".
    mesos::ACL::WaitNestedContainer* acl =
        acls.add_wait_nested_containers();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can wait for all nested containers.
    mesos::ACL::WaitNestedContainer* acl =
        acls.add_wait_nested_containers();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can wait for any nested container.
    mesos::ACL::WaitNestedContainer* acl =
        acls.add_wait_nested_containers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Principal "foo" cannot wait for a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::WAIT_NESTED_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot wait for a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::WAIT_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can wait for a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::WAIT_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can wait for a nested container with a request with
  // ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::WAIT_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can wait for a nested container with a request with
  // an ExecutorInfo without user.
  {
    authorization::Request request;
    request.set_action(authorization::WAIT_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of killing a nested container.
TYPED_TEST(AuthorizationTest, KillNestedContainer)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot kill any nested container.
    mesos::ACL::KillNestedContainer* acl =
        acls.add_kill_nested_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can kill nested containers running under
    // user "bar".
    mesos::ACL::KillNestedContainer* acl =
        acls.add_kill_nested_containers();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can kill all nested containers.
    mesos::ACL::KillNestedContainer* acl =
        acls.add_kill_nested_containers();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can kill any nested container.
    mesos::ACL::KillNestedContainer* acl =
        acls.add_kill_nested_containers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to
  // be authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to
  // be authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Principal "foo" cannot kill a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::KILL_NESTED_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot kill a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::KILL_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can kill a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::KILL_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can kill a nested container with a request with
  // ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::KILL_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can kill a nested container with a request with
  // an ExecutorInfo without user.
  {
    authorization::Request request;
    request.set_action(authorization::KILL_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests the authorization of removing a nested container.
TYPED_TEST(AuthorizationTest, RemoveNestedContainer)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal cannot remove any nested container.
    mesos::ACL::RemoveNestedContainer* acl =
        acls.add_remove_nested_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "bar" principal can remove nested containers running under user "bar".
    mesos::ACL::RemoveNestedContainer* acl =
      acls.add_remove_nested_containers();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_users()->add_values("bar");
  }

  {
    // "ops" principal can remove all nested containers.
    mesos::ACL::RemoveNestedContainer* acl =
      acls.add_remove_nested_containers();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No one else can remove any nested container.
    mesos::ACL::RemoveNestedContainer* acl =
        acls.add_remove_nested_containers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user("user");

  // Create ExecutorInfo with a user not mentioned in the ACLs in
  // command as object to be authorized.
  ExecutorInfo executorInfo;
  {
    executorInfo.set_name("Task");
    executorInfo.mutable_executor_id()->set_value("t");
    executorInfo.mutable_command()->set_value("echo hello");
    executorInfo.mutable_command()->set_user("user");
  }

  // Create ExecutorInfo with user "bar" in command as object to be
  // authorized.
  ExecutorInfo executorInfoBar;
  {
    executorInfoBar.set_name("Executor");
    executorInfoBar.mutable_executor_id()->set_value("e");
    executorInfoBar.mutable_command()->set_value("echo hello");
    executorInfoBar.mutable_command()->set_user("bar");
  }

  // Create ExecutorInfo with no user in command as object to be
  // authorized.
  ExecutorInfo executorInfoNoUser;
  {
    executorInfoNoUser.set_name("Executor");
    executorInfoNoUser.mutable_executor_id()->set_value("e");
    executorInfoNoUser.mutable_command()->set_value("echo hello");
  }

  // Principal "foo" cannot remove a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::REMOVE_NESTED_CONTAINER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "bar" cannot remove a nested container with a request
  // with ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::REMOVE_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "ops" can remove a nested container with a request with
  // ExecutorInfo running with user "user".
  {
    authorization::Request request;
    request.set_action(authorization::REMOVE_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(executorInfo);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can remove a nested container with a request with
  // ExecutorInfo running with user "bar".
  {
    authorization::Request request;
    request.set_action(authorization::REMOVE_NESTED_CONTAINER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoBar);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "ops" can remove nested container with a request with
  // an ExecutorInfo without user.
  {
    authorization::Request request;
    request.set_action(authorization::REMOVE_NESTED_CONTAINER);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->mutable_executor_info()->MergeFrom(
        executorInfoNoUser);
    request.mutable_object()->mutable_framework_info()->MergeFrom(
        frameworkInfo);

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
}


// This tests that a missing request.object is allowed for an ACL whose
// Object is ANY.
// NOTE: The only usecase for this behavior is currently teardownFramework.
TYPED_TEST(AuthorizationTest, OptionalObject)
{
    // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can teardown `ANY` framework
    mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_framework_principals()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No other principal can teardown any framework.
    mesos::ACL::TeardownFramework* acl = acls.add_teardown_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_framework_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Check that principal "foo" can teardown any framework (i.e., a request with
  // missing object).
  {
    authorization::Request request;
    request.set_action(authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "bar" cannot teardown any framework (i.e., a request
  // with missing object).
  {
    authorization::Request request;
    request.set_action(authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, ViewFlags)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_flags()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can see the flags.
    mesos::ACL::ViewFlags* acl = acls.add_view_flags();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_flags()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FLAGS);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_FLAGS);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Test that no authorizer is created with invalid flags.
  {
    ACLs invalid;

    mesos::ACL::ViewFlags* acl = invalid.add_view_flags();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_flags()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


TYPED_TEST(AuthorizationTest, ViewResourceProvider)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view resource provider information.
    mesos::ACL::ViewResourceProvider* acl = acls.add_view_resource_providers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can view resource provider information.
    mesos::ACL::ViewResourceProvider* acl = acls.add_view_resource_providers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_RESOURCE_PROVIDER);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_RESOURCE_PROVIDER);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Test that no authorizer is created with invalid ACLs.
  {
    ACLs invalid;

    mesos::ACL::ViewResourceProvider* acl =
      invalid.add_view_resource_providers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


TYPED_TEST(AuthorizationTest, SetLogLevel)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can set log level.
    mesos::ACL::SetLogLevel* acl = acls.add_set_log_level();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_level()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can set log level.
    mesos::ACL::SetLogLevel* acl = acls.add_set_log_level();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_level()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    authorization::Request request;
    request.set_action(authorization::SET_LOG_LEVEL);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::SET_LOG_LEVEL);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Test that no authorizer is created with invalid ACLs.
  {
    ACLs invalid;

    mesos::ACL::SetLogLevel* acl = invalid.add_set_log_level();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_level()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of ACLs used for unreserve
// operations on dynamically reserved resources.
TYPED_TEST(AuthorizationTest, ValidateEndpoints)
{
  {
    ACLs acls;

    mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_paths()->add_values("/frameworks");

    // Create an `Authorizer` with the ACLs.
    Try<Authorizer*> create = TypeParam::create(parameterize(acls));
    EXPECT_ERROR(create);
  }

  {
    ACLs acls;

    mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_paths()->add_values("/frameworks");
    acl->mutable_paths()->add_values("/monitor/statistics");
    acl->mutable_paths()->add_values("/containers");

    // Create an `Authorizer` with the ACLs.
    Try<Authorizer*> create = TypeParam::create(parameterize(acls));
    EXPECT_ERROR(create);
  }

  {
    ACLs acls;

    mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_paths()->add_values("/monitor/statistics");
    acl->mutable_paths()->add_values("/containers");

    // Create an `Authorizer` with the ACLs.
    Try<Authorizer*> create = TypeParam::create(parameterize(acls));
    ASSERT_SOME(create);
    delete create.get();
  }
}


// This tests the authorization of requests to ViewRole.
TYPED_TEST(AuthorizationTest, ViewRole)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can view `ANY` role.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can view role `bar`.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("bar");
  }

  {
    // "elizabeth-ii" principal can view info about the `king` role and
    // all its nested ones.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // "charles" can only view info about `king`'s nested roles, but not
    // the role `king` itself.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // "j-welby" can only view info about the `king` role itself
    // but not its nested roles.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can view any role.
    mesos::ACL::ViewRole* acl = acls.add_view_roles();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Check that principal "foo" can view any role.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "bar" cannot see role `foo`.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "bar" can see role `bar`.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to view roles in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::VIEW_ROLE);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to UpdateWeight.
TYPED_TEST(AuthorizationTest, UpdateWeight)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can update weights of `ANY` role.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can update the weight of role `bar`.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("bar");
  }

  {
    // "elizabeth-ii" principal can update weights of role `king` and
    // its nested ones.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // "charles" principal can update weights of just roles nested
    // under `king`.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // "j-welby" principal can update the weight of just role 'king'.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can update any weights.
    mesos::ACL::UpdateWeight* acl = acls.add_update_weights();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Check that principal "foo" can update weights of any role.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "bar" cannot update the weight of role `foo`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "bar" can update the weight of role `bar`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can update the weight of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can update the weight of role
  // `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can update the weight of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "charles" cannot update the weight of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "charles" can update the weight of role `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "charles" can update the weight of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" can update the weight of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" cannot update the weight of role
  // `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" cannot update the weight of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_WEIGHT);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization of requests to GetQuota.
TYPED_TEST(AuthorizationTest, GetQuota)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can get quotas of `ANY` role.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can get the quota of role `bar`.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("bar");
  }

  {
    // "elizabeth-ii" principal can view quotas of role `king` and its
    // nested ones.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // "charles" principal can view quotas of just roles nested under `king`.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // "j-welby" principal can view the quotas of just role 'king'.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principal can view any quota.
    mesos::ACL::GetQuota* acl = acls.add_get_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Check that principal "foo" can view quotas of any role.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "bar" cannot view quotas of role `foo`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "bar" can view the quotas of role `bar`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can view the quotas of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can view the quotas of role
  // `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "elizabeth-ii" can view the quota of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "charles" cannot view the quota of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "charles" can view the quota of role `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "charles" can view the quota of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" can view the quota of role `king`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" cannot view the quota of role
  // `king/prince`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Check that principal "j-welby" cannot view the quota of role
  // `king/prince/duke`.
  {
    authorization::Request request;
    request.set_action(authorization::GET_QUOTA);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()->set_value("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, RegisterAgent)
{
  ACLs acls;

  {
    // "foo" principal can register as an agent.
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can register as an agent.
    mesos::ACL::RegisterAgent* acl = acls.add_register_agents();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_agents()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is in the "whitelist".
    authorization::Request request;
    request.set_action(authorization::REGISTER_AGENT);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not in the "whitelist".
    authorization::Request request;
    request.set_action(authorization::REGISTER_AGENT);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::RegisterAgent* acl = invalid.add_register_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to UpdateMaintenanceSchedule.
TYPED_TEST(AuthorizationTest, UpdateMaintenanceSchedule)
{
  ACLs acls;

  {
    // "foo" principal can update maintenance schedule.
    mesos::ACL::UpdateMaintenanceSchedule* acl =
      acls.add_update_maintenance_schedules();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can update maintenance schedule.
    mesos::ACL::UpdateMaintenanceSchedule* acl =
      acls.add_update_maintenance_schedules();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to update maintenance schedules.
    authorization::Request request;
    request.set_action(authorization::UPDATE_MAINTENANCE_SCHEDULE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to update maintenance schedules.
    authorization::Request request;
    request.set_action(authorization::UPDATE_MAINTENANCE_SCHEDULE);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::UpdateMaintenanceSchedule* acl =
      invalid.add_update_maintenance_schedules();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to GetMaintenanceSchedule.
TYPED_TEST(AuthorizationTest, GetMaintenanceSchedule)
{
  ACLs acls;

  {
    // "foo" principal can view the maintenance schedule of the whole cluster.
    mesos::ACL::GetMaintenanceSchedule* acl =
      acls.add_get_maintenance_schedules();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can view the maintenance schedule.
    mesos::ACL::GetMaintenanceSchedule* acl =
      acls.add_get_maintenance_schedules();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to view maintenance schedules. The request should
    // succeed.
    authorization::Request request;
    request.set_action(authorization::GET_MAINTENANCE_SCHEDULE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to view maintenance schedules. The request
    // should fail.
    authorization::Request request;
    request.set_action(authorization::GET_MAINTENANCE_SCHEDULE);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::GetMaintenanceSchedule* acl =
      invalid.add_get_maintenance_schedules();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to StartMaintenance.
TYPED_TEST(AuthorizationTest, StartMaintenance)
{
  ACLs acls;

  {
    // "foo" principal can start maintenance.
    mesos::ACL::StartMaintenance* acl = acls.add_start_maintenances();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can start maintenance.
    mesos::ACL::StartMaintenance* acl = acls.add_start_maintenances();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to start maintenance mode in nodes. The
    // request should succeed.
    authorization::Request request;
    request.set_action(authorization::START_MAINTENANCE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to start maintenance mode in nodes. The
    // request should fail.
    authorization::Request request;
    request.set_action(authorization::START_MAINTENANCE);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::StartMaintenance* acl = invalid.add_start_maintenances();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to StopMaintenance.
TYPED_TEST(AuthorizationTest, StopMaintenance)
{
  ACLs acls;

  {
    // "foo" principal can stop maintenance.
    mesos::ACL::StopMaintenance* acl = acls.add_stop_maintenances();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can stop maintenance.
    mesos::ACL::StopMaintenance* acl = acls.add_stop_maintenances();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to stop maintenance on nodes. The request
    // should succeed.
    authorization::Request request;
    request.set_action(authorization::STOP_MAINTENANCE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to stop maintenance on nodes. The request
    // should fail.
    authorization::Request request;
    request.set_action(authorization::STOP_MAINTENANCE);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::StopMaintenance* acl = invalid.add_stop_maintenances();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to GetMaintenanceStatus.
TYPED_TEST(AuthorizationTest, GetMaintenanceStatus)
{
  ACLs acls;

  {
    // "foo" principal view the maintenance status of the whole cluster.
    mesos::ACL::GetMaintenanceStatus* acl =
      acls.add_get_maintenance_statuses();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can view the maintenance status.
    mesos::ACL::GetMaintenanceStatus* acl =
      acls.add_get_maintenance_statuses();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to view maintenance status. The request should
    // succeed.
    authorization::Request request;
    request.set_action(authorization::GET_MAINTENANCE_STATUS);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to view maintenance status. The request should fail.
    authorization::Request request;
    request.set_action(authorization::GET_MAINTENANCE_STATUS);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::GetMaintenanceStatus* acl =
      invalid.add_get_maintenance_statuses();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_machines()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to DrainAgent.
TYPED_TEST(AuthorizationTest, DrainAgent)
{
  ACLs acls;

  {
    // "foo" principal can drain agents.
    mesos::ACL::DrainAgent* acl = acls.add_drain_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can drain agents.
    mesos::ACL::DrainAgent* acl = acls.add_drain_agents();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_agents()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to drain agents.
    authorization::Request request;
    request.set_action(authorization::DRAIN_AGENT);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to drain agents.
    authorization::Request request;
    request.set_action(authorization::DRAIN_AGENT);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::DrainAgent* acl = invalid.add_drain_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to DeactivateAgent.
TYPED_TEST(AuthorizationTest, DeactivateAgent)
{
  ACLs acls;

  {
    // "foo" principal can deactivate agents.
    mesos::ACL::DeactivateAgent* acl = acls.add_deactivate_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can deactivate agents.
    mesos::ACL::DeactivateAgent* acl = acls.add_deactivate_agents();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_agents()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to deactivate agents.
    authorization::Request request;
    request.set_action(authorization::DEACTIVATE_AGENT);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to deactivate agents.
    authorization::Request request;
    request.set_action(authorization::DEACTIVATE_AGENT);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::DeactivateAgent* acl = invalid.add_deactivate_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to ReactivateAgent.
TYPED_TEST(AuthorizationTest, ReactivateAgent)
{
  ACLs acls;

  {
    // "foo" principal can reactivate agents.
    mesos::ACL::ReactivateAgent* acl = acls.add_reactivate_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can reactivate agents.
    mesos::ACL::ReactivateAgent* acl = acls.add_reactivate_agents();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_agents()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to reactivate agents.
    authorization::Request request;
    request.set_action(authorization::REACTIVATE_AGENT);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to reactivate agents.
    authorization::Request request;
    request.set_action(authorization::REACTIVATE_AGENT);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::ReactivateAgent* acl = invalid.add_reactivate_agents();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_agents()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to ViewStandaloneContainer.
TYPED_TEST(AuthorizationTest, ViewStandaloneContainer)
{
  ACLs acls;

  {
    // "foo" principal can view standalone containers on agents.
    mesos::ACL::ViewStandaloneContainer* acl =
      acls.add_view_standalone_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can view standalone containers.
    mesos::ACL::ViewStandaloneContainer* acl =
      acls.add_view_standalone_containers();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to view standalone containers. The request
    // should succeed.
    authorization::Request request;
    request.set_action(authorization::VIEW_STANDALONE_CONTAINER);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to view standalone containers. The
    // request should fail.
    authorization::Request request;
    request.set_action(authorization::VIEW_STANDALONE_CONTAINER);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::ViewStandaloneContainer* acl =
      invalid.add_view_standalone_containers();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_users()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to MarkResourceProviderGone.
TYPED_TEST(AuthorizationTest, MarkResourceProviderGone)
{
  ACLs acls;

  {
    // "foo" principal can mark resource providers gone.
    mesos::ACL::MarkResourceProvidersGone* acl =
      acls.add_mark_resource_providers_gone();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can mark resource providers gone.
    mesos::ACL::MarkResourceProvidersGone* acl =
      acls.add_mark_resource_providers_gone();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to mark resource providers gone. The request
    // should succeed.
    authorization::Request request;
    request.set_action(authorization::MARK_RESOURCE_PROVIDER_GONE);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to mark resource provider gone. The
    // request should fail.
    authorization::Request request;
    request.set_action(authorization::MARK_RESOURCE_PROVIDER_GONE);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::MarkResourceProvidersGone* acl =
      invalid.add_mark_resource_providers_gone();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to ModifyResourceProviderConfig.
TYPED_TEST(AuthorizationTest, ModifyResourceProviderConfig)
{
  ACLs acls;

  {
    // "foo" principal can modify resource provider configs on agents.
    mesos::ACL::ModifyResourceProviderConfig* acl =
      acls.add_modify_resource_provider_configs();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can modify resource provider configs.
    mesos::ACL::ModifyResourceProviderConfig* acl =
      acls.add_modify_resource_provider_configs();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_resource_providers()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to modify resource provider configs. The request
    // should succeed.
    authorization::Request request;
    request.set_action(authorization::MODIFY_RESOURCE_PROVIDER_CONFIG);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to modify resource provider configs. The
    // request should fail.
    authorization::Request request;
    request.set_action(authorization::MODIFY_RESOURCE_PROVIDER_CONFIG);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::ModifyResourceProviderConfig* acl =
      invalid.add_modify_resource_provider_configs();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_resource_providers()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization of requests to prune images.
TYPED_TEST(AuthorizationTest, PruneImages)
{
  ACLs acls;

  {
    // "foo" principal can prune any images.
    mesos::ACL::PruneImages* acl = acls.add_prune_images();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_images()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can prune images.
    mesos::ACL::PruneImages* acl = acls.add_prune_images();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_images()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    // "foo" is allowed to prune images. This request should succeed.
    authorization::Request request;
    request.set_action(authorization::PRUNE_IMAGES);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    // "bar" is not allowed to prune images. The request should fail.
    authorization::Request request;
    request.set_action(authorization::PRUNE_IMAGES);
    request.mutable_subject()->set_value("bar");

    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    // Test that no authorizer is created with invalid ACLs.
    ACLs invalid;

    mesos::ACL::PruneImages* acl = invalid.add_prune_images();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_images()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}


// This tests the authorization to create block disks.
TYPED_TEST(AuthorizationTest, CreateBlockDisk)
{
  ACLs acls;

  {
    // Principal "foo" can create block disks for any role.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only create block disks for the "panda" role.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot create block disks.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can create block disks for the "king" role
    // and its nested ones.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can create block disks for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can create block disks only for the "king"
    // role but not in any nested one.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can create block disks.
    mesos::ACL::CreateBlockDisk* acl = acls.add_create_block_disks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can create block disks for any role, so this
  // request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can create block disks for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot create block disks for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot create block disks for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to create block disks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization to destroy block disks.
TYPED_TEST(AuthorizationTest, DestroyBlockDisk)
{
  ACLs acls;

  {
    // Principal "foo" can destroy block disks for any role.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only destroy block disks for the "panda" role.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot destroy block disks.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can destroy block disks for the "king" role
    // and its nested ones.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can destroy block disks for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can destroy block disks only for the "king"
    // role but not in any nested one.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can destroy block disks.
    mesos::ACL::DestroyBlockDisk* acl = acls.add_destroy_block_disks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can destroy block disks for any role, so this
  // request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can destroy block disks for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot destroy block disks for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot destroy block disks for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to destroy block disks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_BLOCK_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization to create mount disks.
TYPED_TEST(AuthorizationTest, CreateMountDisk)
{
  ACLs acls;

  {
    // Principal "foo" can create mount disks for any role.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only create mount disks for the "panda" role.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot create mount disks.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can create mount disks for the "king" role
    // and its nested ones.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can create mount disks for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can create mount disks only for the "king" role
    // but not in any nested one.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can create mount disks.
    mesos::ACL::CreateMountDisk* acl = acls.add_create_mount_disks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can create mount disks for any role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can create mount disks for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot create mount disks for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot create mount disks for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to create mount disks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::CREATE_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization to destroy mount disks.
TYPED_TEST(AuthorizationTest, DestroyMountDisk)
{
  ACLs acls;

  {
    // Principal "foo" can destroy mount disks for any role.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only destroy mount disks for the "panda" role.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot destroy mount disks.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can destroy mount disks for the "king" role
    // and its nested ones.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can destroy mount disks for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can destroy mount disks only for the "king"
    // role but not in any nested one.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can destroy mount disks.
    mesos::ACL::DestroyMountDisk* acl = acls.add_destroy_mount_disks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can destroy mount disks for any role, so this
  // request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can destroy mount disks for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot destroy mount disks for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot destroy mount disks for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to destroy mount disks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_MOUNT_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization to destroy raw disks.
TYPED_TEST(AuthorizationTest, DestroyRawDisk)
{
  ACLs acls;

  {
    // Principal "foo" can destroy raw disks for any role.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Principal "bar" can only destroy raw disks for the "panda" role.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("panda");
  }

  {
    // Principal "baz" cannot destroy raw disks.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("baz");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // Principal "elizabeth-ii" can destroy raw disks for the "king" role
    // and its nested ones.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("elizabeth-ii");
    acl->mutable_roles()->add_values("king/%");
    acl->mutable_roles()->add_values("king");
  }

  {
    // Principal "charles" can destroy raw disks for any role below the
    // "king/" role. Not in "king" itself.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("charles");
    acl->mutable_roles()->add_values("king/%");
  }

  {
    // Principal "j-welby" can destroy raw disks only for the "king"
    // role but not in any nested one.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->add_values("j-welby");
    acl->mutable_roles()->add_values("king");
  }

  {
    // No other principals can destroy raw disks.
    mesos::ACL::DestroyRawDisk* acl = acls.add_destroy_raw_disks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can destroy raw disks for any role, so this
  // request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" can destroy raw disks for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // Principal "bar" cannot destroy raw disks for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("giraffe");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "baz" cannot destroy raw disks for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("panda");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // `elizabeth-ii` has full permissions for the `king` role as well as all
  // its nested roles. She should be able to destroy raw disks in the next
  // three blocks.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("elizabeth-ii");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `charles` doesn't have permissions for the `king` role, so the first
  // test should fail. However he has permissions for `king`'s nested roles
  // so the next two tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("charles");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  // `j-welby` only has permissions for the role `king` itself, but not
  // for its nested roles, therefore only the first of the following three
  // tests should succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king");
    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_RAW_DISK);
    request.mutable_subject()->set_value("j-welby");
    request.mutable_object()
      ->mutable_resource()
      ->mutable_reservations()
      ->Add()
      ->set_role("king/prince/duke");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }
}


// This tests the authorization to access Mesos logs.
TYPED_TEST(AuthorizationTest, LogAccess)
{
  // Setup ACLs.
  ACLs acls;

  {
    // "foo" principal can access the logs.
    mesos::ACL::AccessMesosLog* acl = acls.add_access_mesos_logs();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_logs()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Nobody else can access the logs.
    mesos::ACL::AccessMesosLog* acl = acls.add_access_mesos_logs();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_logs()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_MESOS_LOG);
    request.mutable_subject()->set_value("foo");

    AWAIT_EXPECT_TRUE(authorizer->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::ACCESS_MESOS_LOG);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer->authorized(request));
  }

  // Test that no authorizer is created with invalid flags.
  {
    ACLs invalid;

    mesos::ACL::AccessMesosLog* acl = invalid.add_access_mesos_logs();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_logs()->add_values("yoda");

    Try<Authorizer*> create = TypeParam::create(parameterize(invalid));
    EXPECT_ERROR(create);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
