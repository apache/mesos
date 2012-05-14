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

#include <string>

#include <gtest/gtest.h>

#include "tests/base_zookeeper_test.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/group.hpp"


class ZooKeeperTest : public mesos::internal::test::BaseZooKeeperTest {
protected:
  void assertGet(ZooKeeper* client,
                 const std::string& path,
                 const std::string& expected) {
    std::string result;
    ASSERT_EQ(ZOK, client->get(path, false, &result, NULL));
    ASSERT_EQ(expected, result);
  }

  void assertNotSet(ZooKeeper* client,
                    const std::string& path,
                    const std::string& value) {
    ASSERT_EQ(ZNOAUTH, client->set(path, value, -1));
  }
};


TEST_F(ZooKeeperTest, Auth)
{
  mesos::internal::test::BaseZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(zks->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/test",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  assertGet(&authenticatedZk, "/test", "42");

  ZooKeeper unauthenticatedZk(zks->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  assertGet(&unauthenticatedZk, "/test", "42");
  assertNotSet(&unauthenticatedZk, "/test", "37");

  ZooKeeper nonOwnerZk(zks->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  assertGet(&nonOwnerZk, "/test", "42");
  assertNotSet(&nonOwnerZk, "/test", "37");
}


TEST_F(ZooKeeperTest, Group)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  membership.await();

  ASSERT_FALSE(membership.isFailed()) << membership.failure();
  ASSERT_FALSE(membership.isDiscarded());
  ASSERT_TRUE(membership.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(1, memberships.get().size());
  EXPECT_EQ(1, memberships.get().count(membership.get()));

  process::Future<std::string> data = group.data(membership.get());

  data.await();

  ASSERT_FALSE(data.isFailed()) << data.failure();
  ASSERT_FALSE(data.isDiscarded());
  ASSERT_TRUE(data.isReady());
  EXPECT_EQ("hello world", data.get());

  process::Future<bool> cancellation = group.cancel(membership.get());

  cancellation.await();

  ASSERT_FALSE(cancellation.isFailed()) << cancellation.failure();
  ASSERT_FALSE(cancellation.isDiscarded());
  ASSERT_TRUE(cancellation.isReady());
  EXPECT_TRUE(cancellation.get());

  memberships = group.watch(memberships.get());

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(0, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, GroupJoinWithDisconnect)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  zks->shutdownNetwork();

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  EXPECT_TRUE(membership.isPending());

  zks->startNetwork();

  membership.await();

  ASSERT_FALSE(membership.isFailed()) << membership.failure();
  ASSERT_FALSE(membership.isDiscarded());
  ASSERT_TRUE(membership.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(1, memberships.get().size());
  EXPECT_EQ(1, memberships.get().count(membership.get()));
}


TEST_F(ZooKeeperTest, GroupDataWithDisconnect)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  membership.await();

  ASSERT_FALSE(membership.isFailed()) << membership.failure();
  ASSERT_FALSE(membership.isDiscarded());
  ASSERT_TRUE(membership.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(1, memberships.get().size());
  EXPECT_EQ(1, memberships.get().count(membership.get()));

  zks->shutdownNetwork();

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_TRUE(data.isPending());

  zks->startNetwork();

  data.await();

  ASSERT_FALSE(data.isFailed()) << data.failure();
  ASSERT_FALSE(data.isDiscarded());
  ASSERT_TRUE(data.isReady());
  EXPECT_EQ("hello world", data.get());
}


TEST_F(ZooKeeperTest, GroupCancelWithDisconnect)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  membership.await();

  ASSERT_FALSE(membership.isFailed()) << membership.failure();
  ASSERT_FALSE(membership.isDiscarded());
  ASSERT_TRUE(membership.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(1, memberships.get().size());
  EXPECT_EQ(1, memberships.get().count(membership.get()));

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_TRUE(data.isPending());

  data.await();

  ASSERT_FALSE(data.isFailed()) << data.failure();
  ASSERT_FALSE(data.isDiscarded());
  ASSERT_TRUE(data.isReady());
  EXPECT_EQ("hello world", data.get());

  zks->shutdownNetwork();

  process::Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_TRUE(cancellation.isPending());

  zks->startNetwork();

  cancellation.await();

  ASSERT_FALSE(cancellation.isFailed()) << cancellation.failure();
  ASSERT_FALSE(cancellation.isDiscarded());
  ASSERT_TRUE(cancellation.isReady());
  EXPECT_TRUE(cancellation.get());

  memberships = group.watch(memberships.get());

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(0, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, GroupWatchWithSessionExpiration)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  membership.await();

  ASSERT_FALSE(membership.isFailed()) << membership.failure();
  ASSERT_FALSE(membership.isDiscarded());
  ASSERT_TRUE(membership.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(1, memberships.get().size());
  EXPECT_EQ(1, memberships.get().count(membership.get()));

  process::Future<Option<int64_t> > session = group.session();

  session.await();

  ASSERT_FALSE(session.isFailed()) << session.failure();
  ASSERT_FALSE(session.isDiscarded());
  ASSERT_TRUE(session.isReady());
  ASSERT_TRUE(session.get().isSome());

  memberships = group.watch(memberships.get());

  zks->expireSession(session.get().get());

  memberships.await();

  ASSERT_TRUE(memberships.isReady());
  EXPECT_EQ(0, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_FALSE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, MultipleGroups)
{
  zookeeper::Group group1(zks->connectString(), NO_TIMEOUT, "/test/");
  zookeeper::Group group2(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership1 =
    group1.join("group 1");

  membership1.await();

  ASSERT_FALSE(membership1.isFailed()) << membership1.failure();
  ASSERT_FALSE(membership1.isDiscarded());
  ASSERT_TRUE(membership1.isReady());

  process::Future<zookeeper::Group::Membership> membership2 =
    group2.join("group 2");

  membership2.await();

  ASSERT_FALSE(membership2.isFailed()) << membership2.failure();
  ASSERT_FALSE(membership2.isDiscarded());
  ASSERT_TRUE(membership2.isReady());

  process::Future<std::set<zookeeper::Group::Membership> > memberships1 =
    group1.watch();

  memberships1.await();

  ASSERT_TRUE(memberships1.isReady());
  EXPECT_EQ(2, memberships1.get().size());
  EXPECT_EQ(1, memberships1.get().count(membership1.get()));
  EXPECT_EQ(1, memberships1.get().count(membership2.get()));

  process::Future<std::set<zookeeper::Group::Membership> > memberships2 =
    group2.watch();

  memberships2.await();

  ASSERT_TRUE(memberships2.isReady());
  EXPECT_EQ(2, memberships2.get().size());
  EXPECT_EQ(1, memberships2.get().count(membership1.get()));
  EXPECT_EQ(1, memberships2.get().count(membership2.get()));

  process::Future<bool> cancelled;

  // Now watch the membership owned by group1 from group2.
  foreach (const zookeeper::Group::Membership& membership, memberships2.get()) {
    if (membership == membership1.get()) {
      cancelled = membership.cancelled();
      break;
    }
  }

  process::Future<Option<int64_t> > session1 = group1.session();

  session1.await();

  ASSERT_FALSE(session1.isFailed()) << session1.failure();
  ASSERT_FALSE(session1.isDiscarded());
  ASSERT_TRUE(session1.isReady());
  ASSERT_TRUE(session1.get().isSome());

  zks->expireSession(session1.get().get());

  cancelled.await();

  ASSERT_TRUE(cancelled.isReady());
  ASSERT_FALSE(cancelled.get());
}
