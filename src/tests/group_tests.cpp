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

#include <mesos/zookeeper/authentication.hpp>
#include <mesos/zookeeper/group.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>

#include "tests/zookeeper.hpp"

using zookeeper::Group;
using zookeeper::GroupProcess;

using process::Clock;
using process::Future;

using std::set;
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

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  Future<Option<string>> data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  Future<bool> cancellation = group.cancel(membership.get());

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_TRUE(memberships->empty());

  ASSERT_TRUE(membership->cancelled().isReady());
  ASSERT_TRUE(membership->cancelled().get());
}


TEST_F(GroupTest, GroupJoinWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  server->shutdownNetwork();

  Future<Group::Membership> membership = group.join("hello world");

  EXPECT_TRUE(membership.isPending());

  server->startNetwork();

  AWAIT_READY(membership);

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));
}


TEST_F(GroupTest, GroupDataWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  server->shutdownNetwork();

  Future<Option<string>> data = group.data(membership.get());

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

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  AWAIT_EXPECT_TRUE(group.cancel(membership.get()));

  Future<Option<string>> data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_NONE(data.get());
}


TEST_F(GroupTest, GroupCancelWithDisconnect)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  Future<Option<string>> data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  server->shutdownNetwork();

  Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_TRUE(cancellation.isPending());

  server->startNetwork();

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_TRUE(memberships->empty());

  ASSERT_TRUE(membership->cancelled().isReady());
  ASSERT_TRUE(membership->cancelled().get());
}


TEST_F(GroupTest, GroupWatchWithSessionExpiration)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership = group.join("hello world");

  AWAIT_READY(membership);

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  Future<Option<int64_t>> session = group.session();

  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  memberships = group.watch(memberships.get());

  server->expireSession(session->get());

  AWAIT_READY(memberships);
  EXPECT_TRUE(memberships->empty());

  AWAIT_READY(membership->cancelled());
  ASSERT_FALSE(membership->cancelled().get());
}


TEST_F(GroupTest, MultipleGroups)
{
  Group group1(server->connectString(), NO_TIMEOUT, "/test/");
  Group group2(server->connectString(), NO_TIMEOUT, "/test/");

  Future<Group::Membership> membership1 = group1.join("group 1");

  AWAIT_READY(membership1);

  Future<Group::Membership> membership2 = group2.join("group 2");

  AWAIT_READY(membership2);

  Future<set<Group::Membership>> memberships1 = group1.watch();

  AWAIT_READY(memberships1);

  // NOTE: Since 'group2' joining doesn't guarantee that 'group1'
  // knows about it synchronously, we have to ensure that 'group1'
  // knows about both the memberships.
  if (memberships1->size() == 1) {
    memberships1 = group1.watch(memberships1.get());
    AWAIT_READY(memberships1);
  }

  EXPECT_EQ(2u, memberships1->size());
  EXPECT_EQ(1u, memberships1->count(membership1.get()));
  EXPECT_EQ(1u, memberships1->count(membership2.get()));

  Future<set<Group::Membership>> memberships2 = group2.watch();

  AWAIT_READY(memberships2);

  // See comments above for why we need to do this.
  if (memberships2->size() == 1) {
    memberships2 = group2.watch(memberships2.get());
    AWAIT_READY(memberships2);
  }

  EXPECT_EQ(2u, memberships2->size());
  EXPECT_EQ(1u, memberships2->count(membership1.get()));
  EXPECT_EQ(1u, memberships2->count(membership2.get()));

  Future<bool> cancelled;

  // Now watch the membership owned by group1 from group2.
  foreach (const Group::Membership& membership, memberships2.get()) {
    if (membership == membership1.get()) {
      cancelled = membership.cancelled();
      break;
    }
  }

  Future<Option<int64_t>> session1 = group1.session();

  AWAIT_READY(session1);
  ASSERT_SOME(session1.get());

  server->expireSession(session1->get());

  AWAIT_ASSERT_FALSE(cancelled);
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
      nullptr);

  ASSERT_ZK_GET("42", &authenticatedZk, "/read-only");

  authenticatedZk.create(
      "/read-only/writable",
      "37",
      ZOO_OPEN_ACL_UNSAFE,
      0,
      nullptr);

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
  Future<Option<int64_t>> session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  // We repeatedly expire the session while Group operations are
  // on-going. This causes retries of authenticate() and group
  // create().
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session->get());

  Future<Group::Membership> membership = group.join("hello world");

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session->get());

  AWAIT_READY(membership);

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session->get());

  Future<bool> cancellation = group.cancel(membership.get());

  // Wait for Group to connect to get hold of the session.
  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());
  connected = FUTURE_DISPATCH(_, &GroupProcess::connected);
  server->expireSession(session->get());

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

  Future<set<Group::Membership>> memberships = group.watch();

  AWAIT_READY(memberships);
  EXPECT_EQ(1u, memberships->size());
  EXPECT_EQ(1u, memberships->count(membership.get()));

  Future<Option<string>> data = group.data(membership.get());

  AWAIT_READY(data);
  EXPECT_SOME_EQ("hello world", data.get());

  Future<bool> cancellation = group.cancel(membership.get());

  AWAIT_EXPECT_TRUE(cancellation);

  memberships = group.watch(memberships.get());

  AWAIT_READY(memberships);
  EXPECT_TRUE(memberships->empty());

  ASSERT_TRUE(membership->cancelled().isReady());
  ASSERT_TRUE(membership->cancelled().get());
}


// This test checks that the `expired` event is invoked even if we
// have never established a connection to ZooKeeper (MESOS-4546).
TEST_F(GroupTest, ConnectTimer)
{
  const Duration sessionTimeout = Seconds(10);

  Clock::pause();

  // Ensure that we won't be able to establish a connection to ZooKeeper.
  server->shutdownNetwork();

  Group group(server->connectString(), sessionTimeout, "/test/");

  Future<Nothing> expired = FUTURE_DISPATCH(group.process->self(),
                                            &GroupProcess::expired);

  // Advance the clock to ensure that we forcibly expire the current
  // ZooKeeper connection attempt.
  Clock::advance(sessionTimeout);

  AWAIT_READY(expired);

  Clock::resume();
}


// This test checks that if a `Group` is destroyed before the
// connection retry timer fires, we don't attempt to dispatch a
// message to a reclaimed process.
TEST_F(GroupTest, TimerCleanup)
{
  const Duration sessionTimeout = Seconds(10);

  Clock::pause();

  // Ensure that we won't be able to establish a Zk connection.
  server->shutdownNetwork();

  Group* group = new Group(server->connectString(), sessionTimeout, "/test/");

  // We arrange for `sessionTimeout` to expire but we expect that the
  // associated event will not be dispatched. Note that we could
  // specify that we expect which particular PID we expect to not
  // receive any dispatches, but it is a stronger assertion to check
  // that no `GroupProcess` receives an `expired` event.
  EXPECT_NO_FUTURE_DISPATCHES(_, &GroupProcess::expired);

  delete group;

  Clock::advance(sessionTimeout);

  // Ensure that if there are any pending messages, they are delivered.
  Clock::settle();

  Clock::resume();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
