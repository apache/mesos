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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "tests/zookeeper.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/group.hpp"

using zookeeper::Group;
using zookeeper::GroupProcess;

using process::Future;

using std::string;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {

class GroupTest : public ZooKeeperTest {};


TEST_F(GroupTest, Group)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  Future<Option<string> > data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  Future<bool> cancellation = group.cancel(membership.get());

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(GroupTest, GroupJoinWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  server->shutdownNetwork();

  Future<Group::Membership> membership = group.join("hello world");

  EXPECT_TRUE(membership.isPending());

  server->startNetwork();

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));
}


TEST_F(GroupTest, GroupDataWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  server->shutdownNetwork();

  Future<Option<string> > data = group.data(membership.get());

  EXPECT_TRUE(data.isPending());

  server->startNetwork();

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());
}


TEST_F(GroupTest, GroupDataWithRemovedMembership)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  AWAIT_EXPECT_TRUE(group.cancel(membership.get()));

  Future<Option<string> > data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_NONE(data.get());
}


TEST_F(GroupTest, GroupCancelWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  Future<Option<string> > data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  server->shutdownNetwork();

  Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_TRUE(cancellation.isPending());

  server->startNetwork();

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(GroupTest, GroupWatchWithSessionExpiration)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  Future<Option<int64_t> > session = group.session();

  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  memberships = group.watch(memberships.get());

  server->expireSession(session.get().get());

  AWAIT_READY(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  AWAIT_READY(membership.get().cancelled());
  ASSERT_FALSE(membership.get().cancelled().get());
}


TEST_F(GroupTest, MultipleGroups)
{
  Group group1(server->connectString(), NO_TIMEOUT, "/test/");
  Group group2(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership1 = group1.join("group 1");

  AWAIT_READY(membership1);

  Future<Group::Membership> membership2 = group2.join("group 2");

  AWAIT_READY(membership2);

  Future<std::set<Group::Membership> > memberships1 = group1.watch();

  AWAIT_READY(memberships1);

  // NOTE: Since 'group2' joining doesn't guarantee that 'group1'
  // knows about it synchronously, we have to ensure that 'group1'
  // knows about both the memberships.
  if (memberships1.get().size() == 1) {
    memberships1 = group1.watch(memberships1.get());
    AWAIT_READY(memberships1);
  }

  EXPECT_EQ(2u, memberships1.get().size());
  EXPECT_EQ(1u, memberships1.get().count(membership1.get()));
  EXPECT_EQ(1u, memberships1.get().count(membership2.get()));

  Future<std::set<Group::Membership> > memberships2 = group2.watch();

  AWAIT_READY(memberships2);

  // See comments above for why we need to do this.
  if (memberships2.get().size() == 1) {
    memberships2 = group2.watch(memberships2.get());
    AWAIT_READY(memberships2);
  }

  EXPECT_EQ(2u, memberships2.get().size());
  EXPECT_EQ(1u, memberships2.get().count(membership1.get()));
  EXPECT_EQ(1u, memberships2.get().count(membership2.get()));

  Future<bool> cancelled;

  // Now watch the membership owned by group1 from group2.
  foreach (const Group::Membership& membership, memberships2.get()) {
    if (membership == membership1.get()) {
      cancelled = membership.cancelled();
      break;
    }
  }

  Future<Option<int64_t> > session1 = group1.session();

  AWAIT_READY(session1);
  ASSERT_SOME(session1.get());

  server->expireSession(session1.get().get());

  AWAIT_ASSERT_EQ(false, cancelled);
}


TEST_F(GroupTest, GroupPathWithRestrictivePerms)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);

  authenticatedZk.authenticate("digest", "creator:creator");

  authenticatedZk.create(
      "/read-only",
      "42",
      zookeeper::EVERYONE_READ_CREATOR_ALL,
      0,
      NULL);

  ASSERT_ZK_GET("42", &authenticatedZk, "/read-only");

  authenticatedZk.create(
      "/read-only/writable",
      "37",
      ZOO_OPEN_ACL_UNSAFE,
      0,
      NULL);

  ASSERT_ZK_GET("37", &authenticatedZk, "/read-only/writable");

  zookeeper::Authentication auth("digest", "non-creator:non-creator");

  Group failedGroup1(
      server->connectString(),
      NO_TIMEOUT,
      "/read-only/",
      auth);

  AWAIT_FAILED(failedGroup1.join("fail"));

  Group failedGroup2(
      server->connectString(),
      NO_TIMEOUT,
      "/read-only/new",
      auth);

  AWAIT_FAILED(failedGroup2.join("fail"));

  Group successGroup(
      server->connectString(),
      NO_TIMEOUT,
      "/read-only/writable/",
      auth);

  AWAIT_READY(successGroup.join("succeed"));
}


// Verifies that the Group operations can recover from retryable
// errors caused by session expiration. This test does not guarantee
// but rather, attempts to probabilistically trigger the retries with
// repeated session expirations.
TEST_F(GroupTest, RetryableErrors)
{
  Future<Nothing> connected = FUTURE_DISPATCH(_, &GroupProcess::connected);

  zookeeper::Authentication auth("digest", "creator:creator");
  Group group(server->connectString(), NO_TIMEOUT, "/test/", auth);

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  Future<Option<int64_t> > session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  // We repeatedly expire the session while Group operations are
  // on-going. This causes retries of authenticate() and group
  // create().
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session.get().get());

  Future<Group::Membership> membership = group.join("hello world");

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session.get().get());

  AWAIT_READY(membership);

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session.get().get());

  Future<bool> cancellation = group.cancel(membership.get());

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session.get().get());

  AWAIT_READY(cancellation);
  AWAIT_READY(connected);
}


TEST_F(GroupTest, LabelledGroup)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  // Join a group with label.
  Future<Group::Membership> membership = group.join(
      "hello world", string("testlabel"));

  AWAIT_READY(membership);

  Future<std::set<Group::Membership> > memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  Future<Option<string> > data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  Future<bool> cancellation = group.cancel(membership.get());

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
