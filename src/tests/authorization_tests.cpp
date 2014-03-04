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
using namespace mesos::internal;
using namespace mesos::internal::tests;

using namespace process;

class AuthorizationTest : public MesosTest {};


TEST_F(AuthorizationTest, AnyPrincipalRunAsUser)
{
  // Any principal can run as "guest" user.
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->add_values("guest");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "guest".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("guest");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" can run as "root" since the ACLs are permissive.
  mesos::ACL::RunTasks request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("root");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, NoPrincipalRunAsUser)
{
  // No principal can run as "root" user.
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_users()->add_values("root");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" cannot run as "root".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("root");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, PrincipalRunAsAnyUser)
{
  // A principal "foo" can run as any user.
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1" and "user2".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, AnyPrincipalRunAsAnyUser)
{
  // Any principal can run as any user.
  ACLs acls;
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTasks request;
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
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_principals()->add_values("bar");
  acl->mutable_users()->add_values("user1");
  acl->mutable_users()->add_values("user2");

  // ACL for no one else to run as some users.
  mesos::ACL::RunTasks* acl2 = acls.add_run_tasks();
  acl2->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_users()->add_values("user1");
  acl2->mutable_users()->add_values("user2");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can run as "user1" and "user2".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_users()->add_values("user1");
  request.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "baz" cannot run as "user1".
  mesos::ACL::RunTasks request2;
  request2.mutable_principals()->add_values("baz");
  request2.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "baz" cannot run as "user2".
  mesos::ACL::RunTasks request3;
  request3.mutable_principals()->add_values("baz");
  request3.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, SomePrincipalOnlySomeUser)
{
  // Some principal can run as only some user.
  ACLs acls;

  // ACL for some principal to run as some user.
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->add_values("user1");

  // ACL for some principal to not run as any other user.
  mesos::ACL::RunTasks* acl2 = acls.add_run_tasks();
  acl2->mutable_principals()->add_values("foo");
  acl2->mutable_users()->set_type(mesos::ACL::Entity::NONE);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTasks request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" can run as "user1" and "user2".
  mesos::ACL::RunTasks request3;
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
  mesos::ACL::RunTasks* acl = acls.add_run_tasks();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_users()->add_values("user1");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can run as "user1".
  mesos::ACL::RunTasks request;
  request.mutable_principals()->add_values("foo");
  request.mutable_users()->add_values("user1");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "foo" cannot run as "user2".
  mesos::ACL::RunTasks request2;
  request2.mutable_principals()->add_values("foo");
  request2.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" cannot run as "user2" since no ACL is set.
  mesos::ACL::RunTasks request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_users()->add_values("user2");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, AnyPrincipalOfferedRole)
{
  // Any principal can be offered "*" role's resources.
  ACLs acls;
  mesos::ACL::ReceiveOffers* acl = acls.add_receive_offers();
  acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_roles()->add_values("*");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo" and "bar" can be offered "*" role's resources.
  mesos::ACL::ReceiveOffers request;
  request.mutable_principals()->add_values("foo");
  request.mutable_principals()->add_values("bar");
  request.mutable_roles()->add_values("*");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, SomePrincipalsOfferedRole)
{
  // Some principals can be offered "ads" role's resources.
  ACLs acls;
  mesos::ACL::ReceiveOffers* acl = acls.add_receive_offers();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_principals()->add_values("bar");
  acl->mutable_roles()->add_values("ads");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principals "foo", "bar" and "baz" (no ACL) can be offered "ads"
  // role's resources.
  mesos::ACL::ReceiveOffers request;
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
  mesos::ACL::ReceiveOffers* acl = acls.add_receive_offers();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_roles()->add_values("analytics");

  // ACL for no one else to be offered "analytics" role's resources.
  mesos::ACL::ReceiveOffers* acl2 = acls.add_receive_offers();
  acl2->mutable_principals()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_roles()->add_values("analytics");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::ReceiveOffers request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::ReceiveOffers request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, PrincipalNotOfferedAnyRoleRestrictive)
{
  // A principal "foo" can be offered "analytics" role's resources.
  ACLs acls;
  acls.set_permissive(false);
  mesos::ACL::ReceiveOffers* acl = acls.add_receive_offers();
  acl->mutable_principals()->add_values("foo");
  acl->mutable_roles()->add_values("analytics");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Principal "foo" can be offered "analytics" role's resources.
  mesos::ACL::ReceiveOffers request;
  request.mutable_principals()->add_values("foo");
  request.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Principal "bar" cannot be offered "analytics" role's resources.
  mesos::ACL::ReceiveOffers request2;
  request2.mutable_principals()->add_values("bar");
  request2.mutable_roles()->add_values("analytics");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));

  // Principal "bar" cannot be offered "ads" role's resources because no ACL.
  mesos::ACL::ReceiveOffers request3;
  request3.mutable_principals()->add_values("bar");
  request3.mutable_roles()->add_values("ads");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}


TEST_F(AuthorizationTest, AnyClientGETSomeURL)
{
  // Any client can GET access "/help".
  ACLs acls;
  mesos::ACL::HTTPGet* acl = acls.add_http_get();
  acl->mutable_usernames()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_ips()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_hostnames()->set_type(mesos::ACL::Entity::ANY);
  acl->mutable_urls()->add_values("/help");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Clients "foo", "127.0.0.1", "localhost" can GET access "/help".
  mesos::ACL::HTTPGet request;
  request.mutable_usernames()->add_values("foo");
  request.mutable_ips()->add_values("127.0.0.1");
  request.mutable_hostnames()->add_values("localhost");
  request.mutable_urls()->add_values("/help");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, SomeClientsPUTSomeURL)
{
  // Only some clients can PUT access "/admin".
  ACLs acls;

  // Some clients can PUT access "/admin".
  mesos::ACL::HTTPPut* acl = acls.add_http_put();
  acl->mutable_ips()->add_values("127.0.0.1");
  acl->mutable_hostnames()->add_values("localhost");
  acl->mutable_urls()->add_values("/admin");

  // No one else can PUT access "/admin".
  mesos::ACL::HTTPPut* acl2 = acls.add_http_put();
  acl2->mutable_usernames()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_ips()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_hostnames()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_urls()->add_values("/admin");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Clients "127.0.0.1" and "localhost" can PUT access "/admin".
  mesos::ACL::HTTPPut request;
  request.mutable_ips()->add_values("127.0.0.1");
  request.mutable_hostnames()->add_values("localhost");
  request.mutable_urls()->add_values("/admin");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // Client "10.0.0.0" cannot PUT access "/admin".
  mesos::ACL::HTTPPut request2;
  request2.mutable_ips()->add_values("10.0.0.0");
  request2.mutable_urls()->add_values("/admin");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, NoClientGETPUTSomeURL)
{
  // No client can GET access "/secret".
  ACLs acls;
  mesos::ACL::HTTPGet* acl = acls.add_http_get();
  acl->mutable_usernames()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_ips()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_hostnames()->set_type(mesos::ACL::Entity::NONE);
  acl->mutable_urls()->add_values("/secret");

  // No client can PUT access "/secret".
  mesos::ACL::HTTPPut* acl2 = acls.add_http_put();
  acl2->mutable_usernames()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_ips()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_hostnames()->set_type(mesos::ACL::Entity::NONE);
  acl2->mutable_urls()->add_values("/secret");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Clients "127.0.0.1" and "localhost" cannot GET access "/secret".
  mesos::ACL::HTTPGet request;
  request.mutable_ips()->add_values("127.0.0.1");
  request.mutable_hostnames()->add_values("localhost");
  request.mutable_urls()->add_values("/secret");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request));

  // Clients "127.0.0.1" and "localhost" cannot PUT access "/secret".
  mesos::ACL::HTTPPut request2;
  request2.mutable_ips()->add_values("127.0.0.1");
  request2.mutable_hostnames()->add_values("localhost");
  request2.mutable_urls()->add_values("/secret");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, SomeClientsCannotGETAnyURL)
{
  // Some clients cannot GET access any URL.
  ACLs acls;
  mesos::ACL::HTTPGet* acl = acls.add_http_get();
  acl->mutable_ips()->add_values("127.0.0.1");
  acl->mutable_hostnames()->add_values("localhost");
  acl->mutable_urls()->set_type(mesos::ACL::Entity::NONE);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Clients "127.0.0.1" and "localhost" cannot GET access "/help".
  mesos::ACL::HTTPGet request;
  request.mutable_ips()->add_values("127.0.0.1");
  request.mutable_hostnames()->add_values("localhost");
  request.mutable_urls()->add_values("/help");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request));
}


TEST_F(AuthorizationTest, NoClientsCanGETPUTAnyURLRestrictive)
{
  // No clients can GET/PUT access any URL.
  ACLs acls;
  acls.set_permissive(false);

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // Clients "foo", "127.0.0.1" cannot GET access "/help".
  mesos::ACL::HTTPGet request;
  request.mutable_usernames()->add_values("foo");
  request.mutable_ips()->add_values("127.0.0.1");
  request.mutable_urls()->add_values("/help");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request));

  // Clients "127.0.0.1", "localhost" cannot PUT access "/help".
  mesos::ACL::HTTPPut request2;
  request2.mutable_ips()->add_values("127.0.0.1");
  request2.mutable_hostnames()->add_values("localhost");
  request2.mutable_urls()->add_values("/help");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request2));
}


TEST_F(AuthorizationTest, SomeClientsAggregatePUTRequestRestrictive)
{
  // Some clients can PUT access "/admin" but ACLs are setup
  // separately.
  ACLs acls;
  acls.set_permissive(false);

  // "foo" can PUT access "/admin".
  mesos::ACL::HTTPPut* acl = acls.add_http_put();
  acl->mutable_usernames()->add_values("foo");
  acl->mutable_urls()->add_values("/admin");

  // "bar" can PUT access "/admin".
  mesos::ACL::HTTPPut* acl2 = acls.add_http_put();
  acl2->mutable_usernames()->add_values("bar");
  acl2->mutable_urls()->add_values("/admin");

  // Create an Authorizer with the ACLs.
  Try<Owned<LocalAuthorizer> > authorizer = LocalAuthorizer::create(acls);
  ASSERT_SOME(authorizer);

  // "foo" can PUT access "/admin".
  mesos::ACL::HTTPPut request;
  request.mutable_usernames()->add_values("foo");
  request.mutable_urls()->add_values("/admin");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request));

  // "bar" can PUT access "/admin".
  mesos::ACL::HTTPPut request2;
  request2.mutable_usernames()->add_values("bar");
  request2.mutable_urls()->add_values("/admin");
  AWAIT_EXPECT_EQ(true, authorizer.get()->authorize(request2));

  // Aggregate request for clients "foo" and "bar" for PUT access to
  // "/admin" is not allowed because ACLs are not aggregated.
  mesos::ACL::HTTPPut request3;
  request3.mutable_usernames()->add_values("foo");
  request3.mutable_usernames()->add_values("bar");
  request3.mutable_urls()->add_values("/admin");
  AWAIT_EXPECT_EQ(false, authorizer.get()->authorize(request3));
}

