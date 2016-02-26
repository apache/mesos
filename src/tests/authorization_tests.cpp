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

#include <gtest/gtest.h>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/module/authorizer.hpp>

#include "authorizer/local/authorizer.hpp"

#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace tests {

template <typename T>
class AuthorizationTest : public MesosTest {};


typedef ::testing::Types<LocalAuthorizer,
                         tests::Module<Authorizer, TestLocalAuthorizer>>
  AuthorizerTypes;


TYPED_TEST_CASE(AuthorizationTest, AuthorizerTypes);


TYPED_TEST(AuthorizationTest, AnyPrincipalRunAsUser)
{
  // Any principal can run as "guest" user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->add_values("guest");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo" and "bar" can run as "guest".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("guest");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "foo" can run as "root" since the ACLs are permissive.
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("root");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request2));
}


TYPED_TEST(AuthorizationTest, NoPrincipalRunAsUser)
{
  // No principal can run as "root" user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_users()->add_values("root");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" cannot run as "root".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("root");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request));
}


TYPED_TEST(AuthorizationTest, PrincipalRunAsAnyUser)
{
  // A principal "foo" can run as any user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));
}


TYPED_TEST(AuthorizationTest, AnyPrincipalRunAsAnyUser)
{
  // Any principal can run as any user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));
}


TYPED_TEST(AuthorizationTest, OnlySomePrincipalsRunAsSomeUsers)
{
  // Only some principals can run as some users.
  ACLs acls;

  // ACL for some principals to run as some users.
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_principals()->add_values("bar");
  acl->mutable_users()->add_values("user1");
  acl->mutable_users()->add_values("user2");

  // ACL for no one else to run as some users.
  mesos::ACL::RunTask* acl2 = acls.add_run_tasks();
  acl2->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_users()->add_values("user1");
  acl2->mutable_users()->add_values("user2");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "baz" cannot run as "user1".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("baz");
  request2.mutable_users()->add_values("user1");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  // Principal "baz" cannot run as "user2".
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("baz");
  request3.mutable_users()->add_values("user1");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));
}


TYPED_TEST(AuthorizationTest, SomePrincipalOnlySomeUser)
{
  // Some principal can run as only some user.
  ACLs acls;

  // ACL for some principal to run as some user.
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->add_values("user1");

  // ACL for some principal to not run as any other user.
  mesos::ACL::RunTask* acl2 = acls.add_run_tasks();
  acl2->mutable_principals()->add_values("foo");
  acl2->mutable_users()->set_type(mesos::ACL::Entity::NONE);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  // Principal "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_users()->add_values("user1");
  request3.mutable_users()->add_values("user2");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request3));
}


TYPED_TEST(AuthorizationTest, PrincipalRunAsSomeUserRestrictive)
{
  // A principal can run as "user1";
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->add_values("user1");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  // Principal "bar" cannot run as "user2" since no ACL is set.
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_users()->add_values("user2");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));
}


TYPED_TEST(AuthorizationTest, AnyPrincipalOfferedRole)
{
  // Any principal can be offered "*" role's resources.
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_roles()->add_values("*");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo" and "bar" can be offered "*" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_roles()->add_values("*");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));
}


TYPED_TEST(AuthorizationTest, SomePrincipalsOfferedRole)
{
  // Some principals can be offered "ads" role's resources.
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_principals()->add_values("bar");
  acl->mutable_roles()->add_values("ads");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo", "bar" and "baz" (no ACL) can be offered "ads"
  // role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_principals()->add_values("baz");
  request.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));
}


TYPED_TEST(AuthorizationTest, PrincipalOfferedRole)
{
  // Only a principal can be offered "analytics" role's resources.
  ACLs acls;

  // ACL for a principal to be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_roles()->add_values("analytics");

  // ACL for no one else to be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework* acl2 = acls.add_register_frameworks();
  acl2->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_roles()->add_values("analytics");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));
}


TYPED_TEST(AuthorizationTest, PrincipalNotOfferedAnyRoleRestrictive)
{
  // A principal "foo" can be offered "analytics" role's resources.
  ACLs acls;
  acls.set_permissive(false);
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_roles()->add_values("analytics");

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  // Principal "bar" cannot be offered "ads" role's resources because no ACL.
  mesos::ACL::RegisterFramework request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));
}


// Tests the authorization of ACLs used for dynamic reservation of resources.
TYPED_TEST(AuthorizationTest, Reserve)
{
  ACLs acls;

  // Principals "foo" and "bar" can reserve resources for any role.
  mesos::ACL::ReserveResources* acl1 = acls.add_reserve_resources();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_principals()->add_values("bar");
  acl1->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Principal "baz" can only reserve resources for the "ads" role.
  mesos::ACL::ReserveResources* acl2 = acls.add_reserve_resources();
  acl2->mutable_principals()->add_values("baz");
  acl2->mutable_roles()->add_values("ads");

  // No other principals can reserve resources.
  mesos::ACL::ReserveResources* acl3 = acls.add_reserve_resources();
  acl3->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl3->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principals "foo" and "bar" can reserve resources for any role,
  // so requests 1 and 2 will pass.
  mesos::ACL::ReserveResources request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_principals()->add_values("bar");
  request1.mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  mesos::ACL::ReserveResources request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("awesome_role");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request2));

  // Principal "baz" can only reserve resources for the "ads" role, so request 3
  // will pass, but requests 4 and 5 will fail.
  mesos::ACL::ReserveResources request3;
  request3.mutable_principals()->add_values("baz");
  request3.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request3));

  mesos::ACL::ReserveResources request4;
  request4.mutable_principals()->add_values("baz");
  request4.mutable_roles()->add_values("awesome_role");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request4));

  mesos::ACL::ReserveResources request5;
  request5.mutable_principals()->add_values("baz");
  request5.mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request5));

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  mesos::ACL::ReserveResources request6;
  request6.mutable_principals()->add_values("zelda");
  request6.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request6));
}


// This tests the authorization of ACLs used for unreserve
// operations on dynamically reserved resources.
TYPED_TEST(AuthorizationTest, Unreserve)
{
  ACLs acls;

  // "foo" principal can unreserve its own resources.
  mesos::ACL::UnreserveResources* acl1 = acls.add_unreserve_resources();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_reserver_principals()->add_values("foo");

  // "bar" principal cannot unreserve anyone's resources.
  mesos::ACL::UnreserveResources* acl2 = acls.add_unreserve_resources();
  acl2->mutable_principals()->add_values("bar");
  acl2->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  // "ops" principal can unreserve anyone's resources.
  mesos::ACL::UnreserveResources* acl3 = acls.add_unreserve_resources();
  acl3->mutable_principals()->add_values("ops");
  acl3->mutable_reserver_principals()->set_type(mesos::ACL::Entity::ANY);

  // No other principals can unreserve resources.
  mesos::ACL::UnreserveResources* acl4 = acls.add_unreserve_resources();
  acl4->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl4->mutable_reserver_principals()->set_type(mesos::ACL::Entity::NONE);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can unreserve its own resources.
  mesos::ACL::UnreserveResources request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_reserver_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  // Principal "bar" cannot unreserve anyone's
  // resources, so requests 2 and 3 will fail.
  mesos::ACL::UnreserveResources request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_reserver_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  mesos::ACL::UnreserveResources request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_reserver_principals()->add_values("bar");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));

  // Principal "ops" can unreserve anyone's resources,
  // so requests 4 and 5 will succeed.
  mesos::ACL::UnreserveResources request4;
  request4.mutable_principals()->add_values("ops");
  request4.mutable_reserver_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request4));

  mesos::ACL::UnreserveResources request5;
  request5.mutable_principals()->add_values("ops");
  request5.mutable_reserver_principals()->add_values("foo");
  request5.mutable_reserver_principals()->add_values("bar");
  request5.mutable_reserver_principals()->add_values("ops");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request5));

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  mesos::ACL::UnreserveResources request6;
  request6.mutable_principals()->add_values("zelda");
  request6.mutable_reserver_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request6));
}


// Tests the authorization of ACLs used for the creation of persistent volumes.
TYPED_TEST(AuthorizationTest, CreateVolume)
{
  ACLs acls;

  // Principal "foo" can create volumes for any role.
  mesos::ACL::CreateVolume* acl1 = acls.add_create_volumes();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // Principal "bar" can only create volumes for the "panda" role.
  mesos::ACL::CreateVolume* acl2 = acls.add_create_volumes();
  acl2->mutable_principals()->add_values("bar");
  acl2->mutable_roles()->add_values("panda");

  // Principal "baz" cannot create volumes.
  mesos::ACL::CreateVolume* acl3 = acls.add_create_volumes();
  acl3->mutable_principals()->add_values("baz");
  acl3->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // No other principals can create volumes.
  mesos::ACL::CreateVolume* acl4 = acls.add_create_volumes();
  acl4->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl4->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create an Authorizer with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can create volumes for any role, so this request will pass.
  mesos::ACL::CreateVolume request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_roles()->add_values("awesome_role");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  // Principal "bar" can create volumes for the "panda" role,
  // so this request will pass.
  mesos::ACL::CreateVolume request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("panda");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request2));

  // Principal "bar" cannot create volumes for the "giraffe" role,
  // so this request will fail.
  mesos::ACL::CreateVolume request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_roles()->add_values("giraffe");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));

  // Principal "baz" cannot create volumes for any role,
  // so this request will fail.
  mesos::ACL::CreateVolume request4;
  request4.mutable_principals()->add_values("baz");
  request4.mutable_roles()->add_values("panda");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request4));

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This request will fail.
  mesos::ACL::CreateVolume request5;
  request5.mutable_principals()->add_values("zelda");
  request5.mutable_roles()->add_values("panda");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request5));
}


// This tests the authorization of ACLs used for destruction
// operations on persistent volumes.
TYPED_TEST(AuthorizationTest, DestroyVolume)
{
  ACLs acls;

  // "foo" principal can destroy its own volumes.
  mesos::ACL::DestroyVolume* acl1 = acls.add_destroy_volumes();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_creator_principals()->add_values("foo");

  // "bar" principal cannot destroy anyone's volumes.
  mesos::ACL::DestroyVolume* acl2 = acls.add_destroy_volumes();
  acl2->mutable_principals()->add_values("bar");
  acl2->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);

  // "ops" principal can destroy anyone's volumes.
  mesos::ACL::DestroyVolume* acl3 = acls.add_destroy_volumes();
  acl3->mutable_principals()->add_values("ops");
  acl3->mutable_creator_principals()->set_type(mesos::ACL::Entity::ANY);

  // No other principals can destroy volumes.
  mesos::ACL::DestroyVolume* acl4 = acls.add_destroy_volumes();
  acl4->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl4->mutable_creator_principals()->set_type(mesos::ACL::Entity::NONE);

  // Create an Authorizer with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can destroy its own volumes, so this will pass.
  mesos::ACL::DestroyVolume request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_creator_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  // Principal "bar" cannot destroy anyone's
  // volumes, so requests 2 and 3 will fail.
  mesos::ACL::DestroyVolume request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_creator_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  mesos::ACL::DestroyVolume request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_creator_principals()->add_values("bar");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));

  // Principal "ops" can destroy anyone's volumes,
  // so requests 4 and 5 will succeed.
  mesos::ACL::DestroyVolume request4;
  request4.mutable_principals()->add_values("ops");
  request4.mutable_creator_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request4));

  mesos::ACL::DestroyVolume request5;
  request5.mutable_principals()->add_values("ops");
  request5.mutable_creator_principals()->add_values("bar");
  request5.mutable_creator_principals()->add_values("ops");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request5));

  // Principal "zelda" is not mentioned in the ACLs of the Authorizer, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  mesos::ACL::DestroyVolume request6;
  request6.mutable_principals()->add_values("zelda");
  request6.mutable_creator_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request6));
}


// This tests the authorization of requests to set quotas.
TYPED_TEST(AuthorizationTest, SetQuota)
{
  ACLs acls;

  // "foo" principal can set quotas for all roles.
  mesos::ACL::SetQuota* acl1 = acls.add_set_quotas();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

  // "bar" principal can set quotas for "dev" role.
  mesos::ACL::SetQuota* acl2 = acls.add_set_quotas();
  acl2->mutable_principals()->add_values("bar");
  acl2->mutable_roles()->add_values("dev");

  // Anyone can set quotas for "test" role.
  mesos::ACL::SetQuota* acl3 = acls.add_set_quotas();
  acl3->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl3->mutable_roles()->add_values("test");

  // No other principal can set quotas.
  mesos::ACL::SetQuota* acl4 = acls.add_set_quotas();
  acl4->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl4->mutable_roles()->set_type(mesos::ACL::Entity::NONE);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can set quota for all roles, so requests 1 and 2 will pass.
  mesos::ACL::SetQuota request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  mesos::ACL::SetQuota request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_roles()->add_values("prod");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request2));

  // Principal "bar" can set quotas for role "dev", so this will pass.
  mesos::ACL::SetQuota request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_roles()->add_values("dev");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request3));

  // Principal "bar" can only set quotas for role "dev",
  // so request 4 and 5 will fail.
  mesos::ACL::SetQuota request4;
  request4.mutable_principals()->add_values("bar");
  request4.mutable_roles()->add_values("prod");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request4));

  mesos::ACL::SetQuota request5;
  request5.mutable_principals()->add_values("bar");
  request5.mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request5));

  // Anyone can set quotas for role "test", so request 6 will pass.
  mesos::ACL::SetQuota request6;
  request6.mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  request6.mutable_roles()->add_values("test");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request6));

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final ACL, which provides a default case that denies
  // access for all other principals. This case will fail.
  mesos::ACL::SetQuota request7;
  request7.mutable_principals()->add_values("jeff");
  request7.mutable_roles()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request7));
}


// This tests the authorization of requests to remove quotas.
TYPED_TEST(AuthorizationTest, RemoveQuota)
{
  ACLs acls;

  // "foo" principal can remove its own quotas.
  mesos::ACL::RemoveQuota* acl1 = acls.add_remove_quotas();
  acl1->mutable_principals()->add_values("foo");
  acl1->mutable_quota_principals()->add_values("foo");

  // "bar" principal cannot remove anyone's quotas.
  mesos::ACL::RemoveQuota* acl2 = acls.add_remove_quotas();
  acl2->mutable_principals()->add_values("bar");
  acl2->mutable_quota_principals()->set_type(mesos::ACL::Entity::NONE);

  // "ops" principal can remove anyone's quotas.
  mesos::ACL::RemoveQuota* acl3 = acls.add_remove_quotas();
  acl3->mutable_principals()->add_values("ops");
  acl3->mutable_quota_principals()->set_type(mesos::ACL::Entity::ANY);

  // No other principals can remove quotas.
  mesos::ACL::RemoveQuota* acl4 = acls.add_remove_quotas();
  acl4->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl4->mutable_quota_principals()->set_type(mesos::ACL::Entity::NONE);

  // Create an `Authorizer` with the ACLs.
  Try<Authorizer*> create = TypeParam::create();
  ASSERT_SOME(create);
  Owned<Authorizer> authorizer(create.get());

  Try<Nothing> initialized = authorizer.get()->initialize(acls);
  ASSERT_SOME(initialized);

  // Principal "foo" can remove its own quotas, so request 1 will pass.
  mesos::ACL::RemoveQuota request1;
  request1.mutable_principals()->add_values("foo");
  request1.mutable_quota_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request1));

  // Principal "bar" cannot remove anyone's quotas, so requests 2 and 3 will
  // fail.
  mesos::ACL::RemoveQuota request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_quota_principals()->add_values("bar");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request2));

  mesos::ACL::RemoveQuota request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_quota_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request3));

  // Principal "ops" can remove anyone's quotas, so requests 4 and 5 will pass.
  mesos::ACL::RemoveQuota request4;
  request4.mutable_principals()->add_values("ops");
  request4.mutable_quota_principals()->add_values("foo");
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request4));

  mesos::ACL::RemoveQuota request5;
  request5.mutable_principals()->add_values("ops");
  request5.mutable_quota_principals()->set_type(mesos::ACL::Entity::ANY);
  AWAIT_EXPECT_TRUE(authorizer.get()->authorize(request5));

  // Principal "jeff" is not mentioned in the ACLs of the `Authorizer`, so it
  // will be caught by the final rule, which provides a default case that denies
  // access for all other principals. This case will fail.
  mesos::ACL::RemoveQuota request6;
  request6.mutable_principals()->add_values("jeff");
  request6.mutable_quota_principals()->add_values("foo");
  AWAIT_EXPECT_FALSE(authorizer.get()->authorize(request6));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
