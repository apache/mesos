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

#include <zookeeper.h>

#include <string>

#include <gtest/gtest.h>

#include "tests/base_zookeeper_test.hpp"
#include "tests/utils.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/group.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;


class ZooKeeperTest : public BaseZooKeeperTest {
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
  BaseZooKeeperTest::TestWatcher watcher;

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

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_FUTURE_WILL_EQ("hello world", data);

  process::Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_FUTURE_WILL_EQ(true, cancellation);

  memberships = group.watch(memberships.get());

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(0u, memberships.get().size());

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

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));
}


TEST_F(ZooKeeperTest, GroupDataWithDisconnect)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  zks->shutdownNetwork();

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_TRUE(data.isPending());

  zks->startNetwork();

  EXPECT_FUTURE_WILL_EQ("hello world", data);
}


TEST_F(ZooKeeperTest, GroupCancelWithDisconnect)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_FUTURE_WILL_EQ("hello world", data);

  zks->shutdownNetwork();

  process::Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_TRUE(cancellation.isPending());

  zks->startNetwork();

  EXPECT_FUTURE_WILL_EQ(true, cancellation);

  memberships = group.watch(memberships.get());

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, GroupWatchWithSessionExpiration)
{
  zookeeper::Group group(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  process::Future<Option<int64_t> > session = group.session();

  ASSERT_FUTURE_WILL_SUCCEED(session);
  ASSERT_TRUE(session.get().isSome());

  memberships = group.watch(memberships.get());

  zks->expireSession(session.get().get());

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_FALSE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, MultipleGroups)
{
  zookeeper::Group group1(zks->connectString(), NO_TIMEOUT, "/test/");
  zookeeper::Group group2(zks->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership1 =
    group1.join("group 1");

  ASSERT_FUTURE_WILL_SUCCEED(membership1);

  process::Future<zookeeper::Group::Membership> membership2 =
    group2.join("group 2");

  ASSERT_FUTURE_WILL_SUCCEED(membership2);

  process::Future<std::set<zookeeper::Group::Membership> > memberships1 =
    group1.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships1);
  EXPECT_EQ(2u, memberships1.get().size());
  EXPECT_EQ(1u, memberships1.get().count(membership1.get()));
  EXPECT_EQ(1u, memberships1.get().count(membership2.get()));

  process::Future<std::set<zookeeper::Group::Membership> > memberships2 =
    group2.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships2);
  EXPECT_EQ(2u, memberships2.get().size());
  EXPECT_EQ(1u, memberships2.get().count(membership1.get()));
  EXPECT_EQ(1u, memberships2.get().count(membership2.get()));

  process::Future<bool> cancelled;

  // Now watch the membership owned by group1 from group2.
  foreach (const zookeeper::Group::Membership& membership, memberships2.get()) {
    if (membership == membership1.get()) {
      cancelled = membership.cancelled();
      break;
    }
  }

  process::Future<Option<int64_t> > session1 = group1.session();

  ASSERT_FUTURE_WILL_SUCCEED(session1);
  ASSERT_TRUE(session1.get().isSome());

  zks->expireSession(session1.get().get());

  ASSERT_FUTURE_WILL_EQ(false, cancelled);
}


TEST_F(ZooKeeperTest, GroupPathWithRestrictivePerms)
{
  BaseZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(zks->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/read-only",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  assertGet(&authenticatedZk, "/read-only", "42");
  authenticatedZk.create("/read-only/writable",
                         "37",
                         ZOO_OPEN_ACL_UNSAFE,
                         0,
                         NULL);
  assertGet(&authenticatedZk, "/read-only/writable", "37");

  zookeeper::Authentication auth("digest", "non-creator:non-creator");

  zookeeper::Group failedGroup1(zks->connectString(), NO_TIMEOUT,
                                "/read-only/", auth);
  process::Future<zookeeper::Group::Membership> failedMembership1 =
    failedGroup1.join("fail");

  ASSERT_FUTURE_WILL_FAIL(failedMembership1);

  zookeeper::Group failedGroup2(zks->connectString(), NO_TIMEOUT,
                                "/read-only/new", auth);
  process::Future<zookeeper::Group::Membership> failedMembership2 =
    failedGroup2.join("fail");

  ASSERT_FUTURE_WILL_FAIL(failedMembership2);

  zookeeper::Group successGroup(zks->connectString(), NO_TIMEOUT,
                                "/read-only/writable/", auth);
  process::Future<zookeeper::Group::Membership> successMembership =
    successGroup.join("succeed");

  ASSERT_FUTURE_WILL_SUCCEED(successMembership);
}
