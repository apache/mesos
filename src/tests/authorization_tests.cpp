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


typedef ::testing::Types<LocalAuthorizer,
                         tests::Module<Authorizer, TestLocalAuthorizer>>
  AuthorizerTypes;


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

  // Principals "foo" and "bar" can run as "guest".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("guest");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("guest");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "foo" can run as "root" since the ACLs are permissive.
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("root");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("root");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, PrincipalRunAsAnyUser)
{
  // A principal "foo" can run as any user.
  ACLs acls;

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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");;
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
}


TYPED_TEST(AuthorizationTest, AnyPrincipalRunAsAnyUser)
{
  // Any principal can run as any user.
  ACLs acls;

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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "baz" cannot run as "user1".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "baz" cannot run as "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "foo" cannot run as "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can run as "user1" and "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
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
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user1");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "foo" cannot run as "user2".
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot run as "user2" since no ACL is set.
  {
    authorization::Request request;
    request.set_action(authorization::RUN_TASK_WITH_USER);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("user2");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("*");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot be offered "analytics" role's resources.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("analytics");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot be offered "ads" role's resources because no ACL.
  {
    authorization::Request request;
    request.set_action(authorization::REGISTER_FRAMEWORK_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "baz" can only reserve resources for the "ads" role, so request 3
  // will pass, but requests 4 and 5 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("awesome_role");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::RESERVE_RESOURCES_WITH_ROLE);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("ads");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot unreserve anyone's
  // resources, so requests 2 and 3 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "ops" can unreserve anyone's resources,
  // so requests 4 and 5 will succeed.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("ops");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UNRESERVE_RESOURCES_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can create volumes for the "panda" role,
  // so this request will pass.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot create volumes for the "giraffe" role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("giraffe");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "baz" cannot create volumes for any role,
  // so this request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("baz");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  {
    authorization::Request request;
    request.set_action(authorization::CREATE_VOLUME_WITH_ROLE);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("panda");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot destroy anyone's
  // volumes, so requests 2 and 3 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "ops" can destroy anyone's volumes,
  // so requests 4 and 5 will succeed.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("ops");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_VOLUME_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("zelda");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
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
    // No other principal can update quotas.
    mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can update quota for all roles, so requests 1 and 2 will
  // pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can update quotas for role "dev", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("dev");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can only update quotas for role "dev",
  // so requests 4 and 5 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Anyone can update quotas for role "test", so request 6 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_object()->set_value("test");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::UPDATE_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("jeff");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }
}


// This tests that update_quotas and set_quotas/remove_quotas
// cannot be used together.
// TODO(zhitao): Remove this test case at the end of the deprecation
// cycle started with 0.29.
TYPED_TEST(AuthorizationTest, ConflictQuotaACLs) {
  {
    ACLs acls;

    {
      // Add an UpdateQuota ACL.
      mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
      acl->mutable_principals()->add_values("foo");
      acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
    }

    {
      // Add a SetQuota ACL.
      mesos::ACL::SetQuota* acl = acls.add_set_quotas();
      acl->mutable_principals()->add_values("foo");
      acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
    }

    // Create an `Authorizer` with the ACLs should error out.
    Try<Authorizer*> create = TypeParam::create(parameterize(acls));
    ASSERT_ERROR(create);
  }

  {
    ACLs acls;

    {
      // Add an UpdateQuota ACL.
      mesos::ACL::UpdateQuota* acl = acls.add_update_quotas();
      acl->mutable_principals()->add_values("foo");
      acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
    }

    {
      // Add a RemoveQuota ACL.
      mesos::ACL::RemoveQuota* acl = acls.add_remove_quotas();
      acl->mutable_principals()->add_values("foo");
      acl->mutable_quota_principals()->set_type(mesos::ACL::Entity::ANY);
    }

    // Create an `Authorizer` with the ACLs should error out.
    Try<Authorizer*> create = TypeParam::create(parameterize(acls));
    ASSERT_ERROR(create);
  }
}


// This tests the authorization of requests to set quotas.
// TODO(zhitao): Remove this test case at the end of the deprecation
// cycle started with 0.29.
TYPED_TEST(AuthorizationTest, SetQuota)
{
  ACLs acls;

  {
    // "foo" principal can set quotas for all roles.
    mesos::ACL::SetQuota* acl = acls.add_set_quotas();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // "bar" principal can set quotas for "dev" role.
    mesos::ACL::SetQuota* acl = acls.add_set_quotas();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_roles()->add_values("dev");
  }
  {
    // Anyone can set quotas for "test" role.
    mesos::ACL::SetQuota* acl = acls.add_set_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values("test");
  }

  {
    // No other principal can set quotas.
    mesos::ACL::SetQuota* acl = acls.add_set_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can set quota for all roles, so requests 1 and 2 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can set quotas for role "dev", so this will pass.
  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("dev");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" can only set quotas for role "dev",
  // so request 4 and 5 will fail.
  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("prod");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Anyone can set quotas for role "test", so request 6 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_object()->set_value("test");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::SET_QUOTA_WITH_ROLE);
    request.mutable_subject()->set_value("jeff");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }
}


// This tests the authorization of requests to remove quotas.
// TODO(zhitao): Remove this test case at the end of the deprecation
// cycle started with 0.29.
TYPED_TEST(AuthorizationTest, RemoveQuota)
{
  ACLs acls;

  {
    // "foo" principal can remove its own quotas.
    mesos::ACL::RemoveQuota* acl = acls.add_remove_quotas();
    acl->mutable_principals()->add_values("foo");
    acl->mutable_quota_principals()->add_values("foo");
  }

  {
  // "bar" principal cannot remove anyone's quotas.
    mesos::ACL::RemoveQuota* acl = acls.add_remove_quotas();
    acl->mutable_principals()->add_values("bar");
    acl->mutable_quota_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  {
    // "ops" principal can remove anyone's quotas.
    mesos::ACL::RemoveQuota* acl = acls.add_remove_quotas();
    acl->mutable_principals()->add_values("ops");
    acl->mutable_quota_principals()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // No other principals can remove quotas.
    mesos::ACL::RemoveQuota* acl = acls.add_remove_quotas();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_quota_principals()->set_type(mesos::ACL::Entity::NONE);
  }

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create(parameterize(acls));
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  // Principal "foo" can remove its own quotas, so request 1 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("foo");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "bar" cannot remove anyone's quotas, so requests 2 and 3 will
  // fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("bar");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("bar");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }

  // Principal "ops" can remove anyone's quotas, so requests 4 and 5 will pass.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("ops");
    AWAIT_EXPECT_TRUE(authorizer.get()->authorized(request));
  }

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final rule, which provides a default case that denies
  // access for all other principals. This case will fail.
  {
    authorization::Request request;
    request.set_action(authorization::DESTROY_QUOTA_WITH_PRINCIPAL);
    request.mutable_subject()->set_value("jeff");
    request.mutable_object()->set_value("foo");
    AWAIT_EXPECT_FALSE(authorizer.get()->authorized(request));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
