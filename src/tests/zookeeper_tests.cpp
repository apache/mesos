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

#include <gmock/gmock.h>

#include <string>

#include <process/clock.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "messages/messages.hpp"

#include "tests/utils.hpp"
#include "tests/zookeeper_test.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/group.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using process::Clock;

using testing::_;
using testing::Return;


// Helper for invoking ZooKeeper::get(path, ...) in order to check the
// data stored at a specified znode path.
::testing::AssertionResult AssertZKGet(
    const char* expectedExpr,
    const char* zkExpr,
    const char* pathExpr,
    const std::string& expected,
    ZooKeeper* zk,
    const std::string& path)
{
  std::string result;
  int code = zk->get(path, false, &result, NULL);
  if (code == ZOK) {
    if (expected == result) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Expected data at znode '" << pathExpr << "' "
        << "to be '" << expected << "', but actually '" << result << "'";
    }
  } else {
    return ::testing::AssertionFailure()
      << "(" << zkExpr << ").get(" << pathExpr << ", ...): "
      << zk->message(code);
  }
}

#define ASSERT_ZK_GET(expected, zk, path)               \
  ASSERT_PRED_FORMAT3(AssertZKGet, expected, zk, path)


TEST_F(ZooKeeperTest, Auth)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/test",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("42", &authenticatedZk, "/test");

  ZooKeeper unauthenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  ASSERT_ZK_GET("42", &unauthenticatedZk, "/test");
  ASSERT_EQ(ZNOAUTH, unauthenticatedZk.set("/test", "", -1));

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  ASSERT_ZK_GET("42", &nonOwnerZk, "/test");
  ASSERT_EQ(ZNOAUTH, nonOwnerZk.set("/test", "", -1));
}


TEST_F(ZooKeeperTest, Create)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  EXPECT_EQ(ZOK, authenticatedZk.create("/foo/bar",
                                        "",
                                        zookeeper::EVERYONE_READ_CREATOR_ALL,
                                        0,
                                        NULL,
                                        true));
  authenticatedZk.create("/foo/bar/baz",
                         "43",
                         zookeeper::EVERYONE_CREATE_AND_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("43", &authenticatedZk, "/foo/bar/baz");

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  EXPECT_EQ(ZNOAUTH, nonOwnerZk.create("/foo/bar/baz",
                                       "",
                                       zookeeper::EVERYONE_READ_CREATOR_ALL,
                                       0,
                                       NULL,
                                       true));
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/bam",
                                   "44",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   0,
                                   NULL,
                                   true));
  ASSERT_ZK_GET("44", &nonOwnerZk, "/foo/bar/baz/bam");

  std::string result;
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/",
                                   "",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   ZOO_SEQUENCE | ZOO_EPHEMERAL,
                                   &result,
                                   true));
  EXPECT_TRUE(strings::startsWith(result, "/foo/bar/baz/0"));
}


class MockMasterDetectorListenerProcess
  : public ProtobufProcess<MockMasterDetectorListenerProcess>
{
public:
  MockMasterDetectorListenerProcess() {}
  virtual ~MockMasterDetectorListenerProcess() {}

  MOCK_METHOD1(newMasterDetected, void(const process::UPID&));
  MOCK_METHOD0(noMasterDetected, void(void));

protected:
  virtual void initialize()
  {
    install<NewMasterDetectedMessage>(
        &MockMasterDetectorListenerProcess::newMasterDetected,
        &NewMasterDetectedMessage::pid);

    install<NoMasterDetectedMessage>(
        &MockMasterDetectorListenerProcess::noMasterDetected);
  }
};


TEST_F(ZooKeeperTest, MasterDetector)
{
  MockMasterDetectorListenerProcess mock;
  process::spawn(mock);

  trigger newMasterDetectedCall;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(Trigger(&newMasterDetectedCall));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector =
    MasterDetector::create(master, mock.self(), true, true);

  EXPECT_SOME(detector);

  WAIT_UNTIL(newMasterDetectedCall);

  MasterDetector::destroy(detector.get());

  process::terminate(mock);
  process::wait(mock);
}


TEST_F(ZooKeeperTest, MasterDetectors)
{
  MockMasterDetectorListenerProcess mock1;
  process::spawn(mock1);

  trigger newMasterDetectedCall1;
  EXPECT_CALL(mock1, newMasterDetected(mock1.self()))
    .WillOnce(Trigger(&newMasterDetectedCall1));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector1 =
    MasterDetector::create(master, mock1.self(), true, true);

  EXPECT_SOME(detector1);

  WAIT_UNTIL(newMasterDetectedCall1);

  MockMasterDetectorListenerProcess mock2;
  process::spawn(mock2);

  trigger newMasterDetectedCall2;
  EXPECT_CALL(mock2, newMasterDetected(mock1.self())) // N.B. mock1
    .WillOnce(Trigger(&newMasterDetectedCall2));

  Try<MasterDetector*> detector2 =
    MasterDetector::create(master, mock2.self(), true, true);

  EXPECT_SOME(detector2);

  WAIT_UNTIL(newMasterDetectedCall2);

  // Destroying detector1 (below) might cause another election so we
  // need to set up expectations appropriately.
  EXPECT_CALL(mock2, newMasterDetected(_))
    .WillRepeatedly(Return());

  MasterDetector::destroy(detector1.get());

  process::terminate(mock1);
  process::wait(mock1);

  MasterDetector::destroy(detector2.get());

  process::terminate(mock2);
  process::wait(mock2);
}


TEST_F(ZooKeeperTest, MasterDetectorShutdownNetwork)
{
  Clock::pause();

  MockMasterDetectorListenerProcess mock;
  process::spawn(mock);

  trigger newMasterDetectedCall1;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(Trigger(&newMasterDetectedCall1));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector =
    MasterDetector::create(master, mock.self(), true, true);

  EXPECT_SOME(detector);

  WAIT_UNTIL(newMasterDetectedCall1);

  trigger noMasterDetectedCall;
  EXPECT_CALL(mock, noMasterDetected())
    .WillOnce(Trigger(&noMasterDetectedCall));

  server->shutdownNetwork();

  Clock::advance(10.0); // TODO(benh): Get session timeout from detector.

  WAIT_UNTIL(noMasterDetectedCall);

  trigger newMasterDetectedCall2;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(Trigger(&newMasterDetectedCall2));

  server->startNetwork();

  WAIT_FOR(newMasterDetectedCall2, Seconds(5.0));

  MasterDetector::destroy(detector.get());

  process::terminate(mock);
  process::wait(mock);

  Clock::resume();
}


TEST_F(ZooKeeperTest, Group)
{
  zookeeper::Group group(server->connectString(), NO_TIMEOUT, "/test/");

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
  zookeeper::Group group(server->connectString(), NO_TIMEOUT, "/test/");

  server->shutdownNetwork();

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  EXPECT_TRUE(membership.isPending());

  server->startNetwork();

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));
}


TEST_F(ZooKeeperTest, GroupDataWithDisconnect)
{
  zookeeper::Group group(server->connectString(), NO_TIMEOUT, "/test/");

  process::Future<zookeeper::Group::Membership> membership =
    group.join("hello world");

  ASSERT_FUTURE_WILL_SUCCEED(membership);

  process::Future<std::set<zookeeper::Group::Membership> > memberships =
    group.watch();

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(1u, memberships.get().size());
  EXPECT_EQ(1u, memberships.get().count(membership.get()));

  server->shutdownNetwork();

  process::Future<std::string> data = group.data(membership.get());

  EXPECT_TRUE(data.isPending());

  server->startNetwork();

  EXPECT_FUTURE_WILL_EQ("hello world", data);
}


TEST_F(ZooKeeperTest, GroupCancelWithDisconnect)
{
  zookeeper::Group group(server->connectString(), NO_TIMEOUT, "/test/");

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

  server->shutdownNetwork();

  process::Future<bool> cancellation = group.cancel(membership.get());

  EXPECT_TRUE(cancellation.isPending());

  server->startNetwork();

  EXPECT_FUTURE_WILL_EQ(true, cancellation);

  memberships = group.watch(memberships.get());

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_TRUE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, GroupWatchWithSessionExpiration)
{
  zookeeper::Group group(server->connectString(), NO_TIMEOUT, "/test/");

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
  ASSERT_SOME(session.get());

  memberships = group.watch(memberships.get());

  server->expireSession(session.get().get());

  ASSERT_FUTURE_WILL_SUCCEED(memberships);
  EXPECT_EQ(0u, memberships.get().size());

  ASSERT_TRUE(membership.get().cancelled().isReady());
  ASSERT_FALSE(membership.get().cancelled().get());
}


TEST_F(ZooKeeperTest, MultipleGroups)
{
  zookeeper::Group group1(server->connectString(), NO_TIMEOUT, "/test/");
  zookeeper::Group group2(server->connectString(), NO_TIMEOUT, "/test/");

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
  ASSERT_SOME(session1.get());

  server->expireSession(session1.get().get());

  ASSERT_FUTURE_WILL_EQ(false, cancelled);
}


TEST_F(ZooKeeperTest, GroupPathWithRestrictivePerms)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/read-only",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("42", &authenticatedZk, "/read-only");
  authenticatedZk.create("/read-only/writable",
                         "37",
                         ZOO_OPEN_ACL_UNSAFE,
                         0,
                         NULL);
  ASSERT_ZK_GET("37", &authenticatedZk, "/read-only/writable");

  zookeeper::Authentication auth("digest", "non-creator:non-creator");

  zookeeper::Group failedGroup1(server->connectString(), NO_TIMEOUT,
                                "/read-only/", auth);
  process::Future<zookeeper::Group::Membership> failedMembership1 =
    failedGroup1.join("fail");

  ASSERT_FUTURE_WILL_FAIL(failedMembership1);

  zookeeper::Group failedGroup2(server->connectString(), NO_TIMEOUT,
                                "/read-only/new", auth);
  process::Future<zookeeper::Group::Membership> failedMembership2 =
    failedGroup2.join("fail");

  ASSERT_FUTURE_WILL_FAIL(failedMembership2);

  zookeeper::Group successGroup(server->connectString(), NO_TIMEOUT,
                                "/read-only/writable/", auth);
  process::Future<zookeeper::Group::Membership> successMembership =
    successGroup.join("succeed");

  ASSERT_FUTURE_WILL_SUCCEED(successMembership);
}
