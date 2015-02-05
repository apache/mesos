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

#include <gtest/gtest.h>

#include <process/future.hpp>

#include "authorizer/authorizer.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::tests;

using namespace process;

class AuthorizationTest : public MesosTest {};


TEST_F(AuthorizationTest, AnyPrincipalRunAsUser)
{
  // Any principal can run as "guest" user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->add_values("guest");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "guest".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("guest");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" can run as "root" since the ACLs are permissive.
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("root");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, NoPrincipalRunAsUser)
{
  // No principal can run as "root" user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_users()->add_values("root");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" cannot run as "root".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("root");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, PrincipalRunAsAnyUser)
{
  // A principal "foo" can run as any user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, AnyPrincipalRunAsAnyUser)
{
  // Any principal can run as any user.
  ACLs acls;
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, OnlySomePrincipalsRunAsSomeUsers)
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

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "baz" cannot run as "user1".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("baz");
  request2.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "baz" cannot run as "user2".
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("baz");
  request3.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, SomePrincipalOnlySomeUser)
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

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" can run as "user1" and "user2".
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_users()->add_values("user1");
  request3.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, PrincipalRunAsSomeUserRestrictive)
{
  // A principal can run as "user1";
  ACLs acls;
  acls.set_permissive(false); // Restrictive.
  mesos::ACL::RunTask* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->add_values("user1");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTask request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTask request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" cannot run as "user2" since no ACL is set.
  mesos::ACL::RunTask request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, AnyPrincipalOfferedRole)
{
  // Any principal can be offered "*" role's resources.
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_roles()->add_values("*");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can be offered "*" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_roles()->add_values("*");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, SomePrincipalsOfferedRole)
{
  // Some principals can be offered "ads" role's resources.
  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_principals()->add_values("bar");
  acl->mutable_roles()->add_values("ads");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo", "bar" and "baz" (no ACL) can be offered "ads"
  // role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_principals()->add_values("baz");
  request.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, PrincipalOfferedRole)
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

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, PrincipalNotOfferedAnyRoleRestrictive)
{
  // A principal "foo" can be offered "analytics" role's resources.
  ACLs acls;
  acls.set_permissive(false);
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_roles()->add_values("analytics");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::RegisterFramework request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" cannot be offered "ads" role's resources because no ACL.
  mesos::ACL::RegisterFramework request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}
