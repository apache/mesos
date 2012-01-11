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

  process::Future<std::string> info = group.info(membership.get());

  info.await();

  ASSERT_FALSE(info.isFailed()) << info.failure();
  ASSERT_FALSE(info.isDiscarded());
  ASSERT_TRUE(info.isReady());
  EXPECT_EQ("hello world", info.get());

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


TEST_F(ZooKeeperTest, GroupInfoWithDisconnect)
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

  process::Future<std::string> info = group.info(membership.get());

  EXPECT_TRUE(info.isPending());

  zks->startNetwork();

  info.await();

  ASSERT_FALSE(info.isFailed()) << info.failure();
  ASSERT_FALSE(info.isDiscarded());
  ASSERT_TRUE(info.isReady());
  EXPECT_EQ("hello world", info.get());
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

  process::Future<std::string> info = group.info(membership.get());

  EXPECT_TRUE(info.isPending());

  info.await();

  ASSERT_FALSE(info.isFailed()) << info.failure();
  ASSERT_FALSE(info.isDiscarded());
  ASSERT_TRUE(info.isReady());
  EXPECT_EQ("hello world", info.get());

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
}
