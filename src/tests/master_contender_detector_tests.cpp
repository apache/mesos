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

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/zookeeper/contender.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/uri.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "master/contender/standalone.hpp"
#include "master/contender/zookeeper.hpp"

#include "master/detector/standalone.hpp"
#include "master/detector/zookeeper.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"
#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace zookeeper;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::internal::protobuf::createMasterInfo;

using mesos::master::contender::MASTER_CONTENDER_ZK_SESSION_TIMEOUT;
using mesos::master::contender::MasterContender;
using mesos::master::contender::StandaloneMasterContender;
using mesos::master::contender::ZooKeeperMasterContender;

using mesos::master::detector::MASTER_DETECTOR_ZK_SESSION_TIMEOUT;
using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;
using mesos::master::detector::ZooKeeperMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class MasterContenderDetectorTest : public MesosTest {};


TEST_F(MasterContenderDetectorTest, File)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Write "master" to a file and use the "file://" mechanism to
  // create a master detector for the slave. Still requires a master
  // detector for the master first.
  slave::Flags flags = CreateSlaveFlags();

  const string& path = path::join(flags.work_dir, "master");
  ASSERT_SOME(os::write(path, stringify(master.get()->pid)));

  Try<MasterDetector*> _detector = MasterDetector::create(uri::from_path(path));

  ASSERT_SOME(_detector);

  Owned<MasterDetector> detector(_detector.get());

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  driver.stop();
  driver.join();
}


TEST(BasicMasterContenderDetectorTest, Contender)
{
  PID<Master> master;
  master.address.ip = net::IP(10000000);
  master.address.port = 10000;

  Owned<MasterContender> contender(new StandaloneMasterContender());

  contender->initialize(createMasterInfo(master));

  Future<Future<Nothing>> contended = contender->contend();
  AWAIT_READY(contended);

  Future<Nothing> lostCandidacy = contended.get();

  // The candidacy is never lost.
  EXPECT_TRUE(lostCandidacy.isPending());

  contender.reset();

  // Deleting the contender also withdraws the previous candidacy.
  AWAIT_READY(lostCandidacy);
}


TEST(BasicMasterContenderDetectorTest, Detector)
{
  PID<Master> master;
  master.address.ip = net::IP(10000000);
  master.address.port = 10000;

  StandaloneMasterDetector detector;

  Future<Option<MasterInfo>> detected = detector.detect();

  // No one has appointed the leader so we are pending.
  EXPECT_TRUE(detected.isPending());

  // Ensure that the future can be discarded.
  detected.discard();

  AWAIT_DISCARDED(detected);

  detected = detector.detect();

  // Still no leader appointed yet.
  EXPECT_TRUE(detected.isPending());

  detector.appoint(master);

  AWAIT_READY(detected);
}


TEST(BasicMasterContenderDetectorTest, MasterInfo)
{
  PID<Master> master;
  master.address.ip = net::IP::parse("10.10.1.105", AF_INET).get();
  master.address.port = 8088;

  StandaloneMasterDetector detector;

  Future<Option<MasterInfo>> detected = detector.detect();

  detector.appoint(master);

  AWAIT_READY(detected);
  ASSERT_SOME(detected.get());
  const MasterInfo& info = detected->get();

  ASSERT_TRUE(info.has_address());
  EXPECT_EQ("10.10.1.105", info.address().ip());
  ASSERT_EQ(8088, info.address().port());
}


#ifdef MESOS_HAS_JAVA
class ZooKeeperMasterContenderDetectorTest : public ZooKeeperTest {};


// A single contender gets elected automatically.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterContender)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Owned<zookeeper::Group> group(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterContender contender(group);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo master = createMasterInfo(pid);

  contender.initialize(master);
  Future<Future<Nothing>> contended = contender.contend();
  AWAIT_READY(contended);

  ZooKeeperMasterDetector detector(url.get());

  Future<Option<MasterInfo>> leader = detector.detect();

  AWAIT_READY(leader);
  EXPECT_SOME_EQ(master, leader.get());

  leader = detector.detect(leader.get());

  // No change to leadership.
  ASSERT_TRUE(leader.isPending());

  // Ensure we can discard the future.
  leader.discard();

  AWAIT_DISCARDED(leader);

  // After the discard, we can re-detect correctly.
  leader = detector.detect(None());

  AWAIT_READY(leader);
  EXPECT_SOME_EQ(master, leader.get());

  // Now test that a session expiration causes candidacy to be lost
  // and the future to become ready.
  Future<Nothing> lostCandidacy = contended.get();
  leader = detector.detect(leader.get());

  Future<Option<int64_t>> sessionId = group.get()->session();
  AWAIT_READY(sessionId);
  server->expireSession(sessionId->get());

  AWAIT_READY(lostCandidacy);
  AWAIT_READY(leader);
  EXPECT_NONE(leader.get());
}


// Verifies that contender does not recontend if the current election
// is still pending.
TEST_F(ZooKeeperMasterContenderDetectorTest, ContenderPendingElection)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  ZooKeeperMasterContender contender(url.get());

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo master = createMasterInfo(pid);

  contender.initialize(master);

  // Drop Group::join so that 'contended' will stay pending.
  Future<Nothing> join = DROP_DISPATCH(_, &GroupProcess::join);

  Future<Future<Nothing>> contended = contender.contend();
  AWAIT_READY(join);

  Clock::pause();

  // Make sure GroupProcess::join is dispatched (and dropped).
  Clock::settle();

  EXPECT_TRUE(contended.isPending());

  process::filter(nullptr);

  process::TestsFilter* filter =
    process::FilterTestEventListener::instance()->install();

  synchronized (filter->mutex) {
    // Expect GroupProcess::join not getting called because
    // ZooKeeperMasterContender directly returns.
    EXPECT_CALL(
        filter->mock,
        filter(testing::A<const UPID&>(),
               testing::A<const process::DispatchEvent&>()))
      .With(DispatchMatcher(&GroupProcess::join))
      .Times(0);
  }

  // Recontend and settle so that if ZooKeeperMasterContender is not
  // directly returning, GroupProcess::join is dispatched.
  contender.contend();
  Clock::settle();

  Clock::resume();
}


// Two contenders, the first wins. Kill the first, then the second
// is elected.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterContenders)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Owned<ZooKeeperMasterContender> contender1(
      new ZooKeeperMasterContender(url.get()));

  PID<Master> pid1;
  pid1.address.ip = net::IP(10000000);
  pid1.address.port = 10000;

  MasterInfo master1 = createMasterInfo(pid1);

  contender1->initialize(master1);

  Future<Future<Nothing>> contended1 = contender1->contend();
  AWAIT_READY(contended1);

  ZooKeeperMasterDetector detector1(url.get());

  Future<Option<MasterInfo>> leader1 = detector1.detect();
  AWAIT_READY(leader1);
  EXPECT_SOME_EQ(master1, leader1.get());

  ZooKeeperMasterContender contender2(url.get());

  PID<Master> pid2;
  pid2.address.ip = net::IP(10000001);
  pid2.address.port = 10001;

  MasterInfo master2 = createMasterInfo(pid2);

  contender2.initialize(master2);

  Future<Future<Nothing>> contended2 = contender2.contend();
  AWAIT_READY(contended2);

  ZooKeeperMasterDetector detector2(url.get());
  Future<Option<MasterInfo>> leader2 = detector2.detect();
  AWAIT_READY(leader2);
  EXPECT_SOME_EQ(master1, leader2.get());

  LOG(INFO) << "Killing the leading master";

  // Destroying detector1 (below) causes leadership change.
  contender1.reset();

  Future<Option<MasterInfo>> leader3 = detector2.detect(master1);
  AWAIT_READY(leader3);
  EXPECT_SOME_EQ(master2, leader3.get());
}


// Verifies that contender and detector operations fail when facing
// non-retryable errors returned by ZooKeeper.
TEST_F(ZooKeeperMasterContenderDetectorTest, NonRetryableFrrors)
{
  // group1 creates a base directory in ZooKeeper and sets the
  // credential for the user.
  zookeeper::Group group1(
      server->connectString(),
      MASTER_CONTENDER_ZK_SESSION_TIMEOUT,
      "/mesos",
      zookeeper::Authentication("digest", "member:member"));
  AWAIT_READY(group1.join("data"));

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo master = createMasterInfo(pid);

  // group2's password is wrong and operations on it will fail.
  Owned<zookeeper::Group> group2(new Group(
      server->connectString(),
      MASTER_CONTENDER_ZK_SESSION_TIMEOUT,
      "/mesos",
      zookeeper::Authentication("digest", "member:wrongpass")));
  ZooKeeperMasterContender contender(group2);
  contender.initialize(master);

  // Fails due to authentication error.
  AWAIT_FAILED(contender.contend());

  // Subsequent call should also fail.
  AWAIT_FAILED(contender.contend());

  // Now test non-retryable failures in detection.
  ZooKeeperTest::TestWatcher watcher;
  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");

  // Creator of the base path restricts the all accesses to be
  // itself.
  ::ACL onlyCreatorCanAccess[] = {{ ZOO_PERM_ALL, ZOO_AUTH_IDS }};
  authenticatedZk.create("/test",
                         "42",
                         (ACL_vector) {1, onlyCreatorCanAccess},
                         0,
                         nullptr);
  ASSERT_ZK_GET("42", &authenticatedZk, "/test");

  // group3 cannot read the base path thus detector should fail.
  Owned<Group> group3(new Group(
      server->connectString(),
      MASTER_DETECTOR_ZK_SESSION_TIMEOUT,
      "/test",
      None()));

  ZooKeeperMasterDetector detector(group3);
  AWAIT_FAILED(detector.detect());

  // Subsequent call should also fail.
  AWAIT_FAILED(detector.detect());
}


// Master contention and detection fail when the network is down, it
// recovers when the network is back up.
TEST_F(ZooKeeperMasterContenderDetectorTest, ContenderDetectorShutdownNetwork)
{
  Clock::pause();

  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Duration sessionTimeout = process::TEST_AWAIT_TIMEOUT;

  ZooKeeperMasterContender contender(url.get(), sessionTimeout);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo master = createMasterInfo(pid);

  contender.initialize(master);

  Future<Future<Nothing>> contended = contender.contend();
  AWAIT_READY(contended);
  Future<Nothing> lostCandidacy = contended.get();

  ZooKeeperMasterDetector detector(url.get(), sessionTimeout);

  Future<Option<MasterInfo>> leader = detector.detect();
  AWAIT_READY(leader);
  EXPECT_SOME_EQ(master, leader.get());

  leader = detector.detect(leader.get());

  // Shut down ZooKeeper and expect things to fail after the timeout.
  server->shutdownNetwork();

  // We may need to advance multiple times because we could have
  // advanced the clock before the timer in Group starts.
  while (lostCandidacy.isPending() || leader.isPending()) {
    Clock::advance(sessionTimeout);
    Clock::settle();
  }

  // Local timeout does not fail the future but rather deems the
  // session has timed out and the candidacy is lost.
  EXPECT_TRUE(lostCandidacy.isReady());
  EXPECT_NONE(leader.get());

  // Re-contend and re-detect.
  contended = contender.contend();
  leader = detector.detect(leader.get());

  // Things will not change until the server restarts.
  Clock::advance(Minutes(1));
  Clock::settle();
  EXPECT_TRUE(contended.isPending());
  EXPECT_TRUE(leader.isPending());

  server->startNetwork();

  // Operations will eventually succeed after ZK is restored.
  AWAIT_READY(contended);
  AWAIT_READY(leader);

  Clock::resume();
}


// Tests that detectors do not fail when we reach our ZooKeeper
// session timeout.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterDetectorTimedoutSession)
{
  // Use an arbitrary timeout value.
  Duration sessionTimeout(Seconds(10));

  Try<zookeeper::URL> url = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  Owned<zookeeper::Group> leaderGroup(new Group(url.get(), sessionTimeout));

  // First we bring up three master contender/detector:
  //   1. A leading contender.
  //   2. A non-leading contender.
  //   3. A non-contender (detector).

  // 1. Simulate a leading contender.
  ZooKeeperMasterContender leaderContender(leaderGroup);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo leader = createMasterInfo(pid);

  leaderContender.initialize(leader);

  Future<Future<Nothing>> contended = leaderContender.contend();
  AWAIT_READY(contended);
  Future<Nothing> leaderLostCandidacy = contended.get();

  ZooKeeperMasterDetector leaderDetector(leaderGroup);

  Future<Option<MasterInfo>> detected = leaderDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 2. Simulate a non-leading contender.
  Owned<zookeeper::Group> followerGroup(new Group(url.get(), sessionTimeout));
  ZooKeeperMasterContender followerContender(followerGroup);

  PID<Master> pid2;
  pid2.address.ip = net::IP(10000001);
  pid2.address.port = 10001;

  MasterInfo follower = createMasterInfo(pid2);

  followerContender.initialize(follower);

  contended = followerContender.contend();
  AWAIT_READY(contended);
  Future<Nothing> followerLostCandidacy = contended.get();

  ZooKeeperMasterDetector followerDetector(followerGroup);

  detected = followerDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 3. Simulate a non-contender.
  Owned<zookeeper::Group> nonContenderGroup(
      new Group(url.get(), sessionTimeout));
  ZooKeeperMasterDetector nonContenderDetector(nonContenderGroup);

  detected = nonContenderDetector.detect();

  EXPECT_SOME_EQ(leader, detected.get());

  // Expecting the reconnecting event after we shut down the ZK.
  Future<Nothing> leaderReconnecting = FUTURE_DISPATCH(
      leaderGroup->process->self(),
      &GroupProcess::reconnecting);

  Future<Nothing> followerReconnecting = FUTURE_DISPATCH(
      followerGroup->process->self(),
      &GroupProcess::reconnecting);

  Future<Nothing> nonContenderReconnecting = FUTURE_DISPATCH(
      nonContenderGroup->process->self(),
      &GroupProcess::reconnecting);

  server->shutdownNetwork();

  AWAIT_READY(leaderReconnecting);
  AWAIT_READY(followerReconnecting);
  AWAIT_READY(nonContenderReconnecting);

  // Now the detectors re-detect.
  Future<Option<MasterInfo>> leaderDetected =
    leaderDetector.detect(leader);
  Future<Option<MasterInfo>> followerDetected =
    followerDetector.detect(leader);
  Future<Option<MasterInfo>> nonContenderDetected =
    nonContenderDetector.detect(leader);

  Clock::pause();

  // We may need to advance multiple times because we could have
  // advanced the clock before the timer in Group starts.
  while (leaderDetected.isPending() ||
         followerDetected.isPending() ||
         nonContenderDetected.isPending() ||
         leaderLostCandidacy.isPending() ||
         followerLostCandidacy.isPending()) {
    Clock::advance(sessionTimeout);
    Clock::settle();
  }

  EXPECT_NONE(leaderDetected.get());
  EXPECT_NONE(followerDetected.get());
  EXPECT_NONE(nonContenderDetected.get());

  EXPECT_TRUE(leaderLostCandidacy.isReady());
  EXPECT_TRUE(followerLostCandidacy.isReady());

  Clock::resume();
}


// Tests whether a leading master correctly detects a new master when
// its ZooKeeper session is expired (the follower becomes the new
// leader).
TEST_F(ZooKeeperMasterContenderDetectorTest,
       MasterDetectorExpireMasterZKSession)
{
  // Simulate a leading master.
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo leader = createMasterInfo(pid);

  // Create the group instance so we can expire its session.
  Owned<zookeeper::Group> group(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterContender leaderContender(group);

  leaderContender.initialize(leader);

  Future<Future<Nothing>> leaderContended = leaderContender.contend();
  AWAIT_READY(leaderContended);

  Future<Nothing> leaderLostLeadership = leaderContended.get();

  ZooKeeperMasterDetector leaderDetector(url.get());

  Future<Option<MasterInfo>> detected = leaderDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // Keep detecting.
  Future<Option<MasterInfo>> newLeaderDetected =
    leaderDetector.detect(detected.get());

  // Simulate a following master.
  PID<Master> pid2;
  pid2.address.ip = net::IP(10000001);
  pid2.address.port = 10001;

  MasterInfo follower = createMasterInfo(pid2);

  ZooKeeperMasterDetector followerDetector(url.get());
  ZooKeeperMasterContender followerContender(url.get());
  followerContender.initialize(follower);

  Future<Future<Nothing>> followerContended = followerContender.contend();
  AWAIT_READY(followerContended);

  LOG(INFO) << "The follower now is detecting the leader";
  detected = followerDetector.detect(None());
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // Now expire the leader's zk session.
  Future<Option<int64_t>> session = group->session();
  AWAIT_READY(session);
  EXPECT_SOME(session.get());

  LOG(INFO) << "Now expire the ZK session: " << std::hex << session->get();

  server->expireSession(session->get());

  AWAIT_READY(leaderLostLeadership);

  // Wait for session expiration and ensure the former leader detects
  // a new leader.
  AWAIT_READY(newLeaderDetected);
  EXPECT_SOME(newLeaderDetected.get());
  EXPECT_EQ(follower, newLeaderDetected->get());
}


// Tests whether a slave correctly DOES NOT disconnect from the
// master when its ZooKeeper session is expired, but the master still
// stays the leader when the slave re-connects with the ZooKeeper.
TEST_F(ZooKeeperMasterContenderDetectorTest, MasterDetectorExpireSlaveZKSession)
{
  // Simulate a leading master.
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo master = createMasterInfo(pid);

  ZooKeeperMasterContender masterContender(url.get());
  masterContender.initialize(master);

  Future<Future<Nothing>> leaderContended = masterContender.contend();
  AWAIT_READY(leaderContended);

  // Simulate a slave.
  Owned<zookeeper::Group> group(
        new Group(url.get(), MASTER_DETECTOR_ZK_SESSION_TIMEOUT));

  ZooKeeperMasterDetector slaveDetector(group);

  Future<Option<MasterInfo>> detected = slaveDetector.detect();
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(master, detected.get());

  detected = slaveDetector.detect(master);

  // Now expire the slave's zk session.
  Future<Option<int64_t>> session = group->session();
  AWAIT_READY(session);

  server->expireSession(session->get());

  // Session expiration causes detector to assume all membership are
  // lost.
  AWAIT_READY(detected);
  EXPECT_NONE(detected.get());

  detected = slaveDetector.detect(detected.get());

  // The detector is able re-detect the master.
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(master, detected.get());
}


// Tests whether a slave correctly detects the new master when its
// ZooKeeper session is expired and a new master is elected before the
// slave reconnects with ZooKeeper.
TEST_F(ZooKeeperMasterContenderDetectorTest,
       MasterDetectorExpireSlaveZKSessionNewMaster)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  // Simulate a leading master.
  Owned<zookeeper::Group> leaderGroup(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  // 1. Simulate a leading contender.
  ZooKeeperMasterContender leaderContender(leaderGroup);
  ZooKeeperMasterDetector leaderDetector(leaderGroup);

  PID<Master> pid;
  pid.address.ip = net::IP(10000000);
  pid.address.port = 10000;

  MasterInfo leader = createMasterInfo(pid);

  leaderContender.initialize(leader);

  Future<Future<Nothing>> contended = leaderContender.contend();
  AWAIT_READY(contended);

  Future<Option<MasterInfo>> detected = leaderDetector.detect(None());
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(leader, detected.get());

  // 2. Simulate a non-leading contender.
  Owned<zookeeper::Group> followerGroup(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));
  ZooKeeperMasterContender followerContender(followerGroup);
  ZooKeeperMasterDetector followerDetector(followerGroup);

  PID<Master> pid2;
  pid2.address.ip = net::IP(10000001);
  pid2.address.port = 10001;

  MasterInfo follower = createMasterInfo(pid2);

  followerContender.initialize(follower);

  contended = followerContender.contend();
  AWAIT_READY(contended);

  detected = followerDetector.detect(None());
  EXPECT_SOME_EQ(leader, detected.get());

  // 3. Simulate a non-contender.
  Owned<zookeeper::Group> nonContenderGroup(
      new Group(url.get(), MASTER_DETECTOR_ZK_SESSION_TIMEOUT));
  ZooKeeperMasterDetector nonContenderDetector(nonContenderGroup);

  detected = nonContenderDetector.detect();

  EXPECT_SOME_EQ(leader, detected.get());

  detected = nonContenderDetector.detect(leader);

  // Now expire the slave's and leading master's zk sessions.
  // NOTE: Here we assume that slave stays disconnected from the ZK
  // when the leading master loses its session.
  Future<Option<int64_t>> slaveSession = nonContenderGroup->session();
  AWAIT_READY(slaveSession);

  Future<Option<int64_t>> masterSession = leaderGroup->session();
  AWAIT_READY(masterSession);

  server->expireSession(slaveSession->get());
  server->expireSession(masterSession->get());

  // Wait for session expiration and the detector will first receive
  // a "no master detected" event.
  AWAIT_READY(detected);
  EXPECT_NONE(detected.get());

  // nonContenderDetector can now re-detect the new master.
  detected = nonContenderDetector.detect(detected.get());
  AWAIT_READY(detected);
  EXPECT_SOME_EQ(follower, detected.get());
}


TEST_F(ZooKeeperMasterContenderDetectorTest, MasterDetectorUsesJson)
{
  Try<zookeeper::URL> url = zookeeper::URL::parse(
      "zk://" + server->connectString() + "/mesos");

  ASSERT_SOME(url);

  // Construct a known MasterInfo & convert into JSON.
  PID<Master> pid;
  pid.address.ip = net::IP::parse("10.10.1.22", AF_INET).get();
  pid.address.port = 8080;

  MasterInfo leader = createMasterInfo(pid);
  JSON::Object masterInfo = JSON::protobuf(leader);

  // Simulate a leading master.
  Owned<zookeeper::Group> group(
      new Group(url.get(), MASTER_CONTENDER_ZK_SESSION_TIMEOUT));

  // Simulate a leading contender: we need to force-write JSON
  // here, as this is still not supported in Mesos 0.23.
  LeaderContender contender(
      group.get(), stringify(masterInfo), master::MASTER_INFO_JSON_LABEL);
  Option<Future<Future<Nothing>>> candidacy = contender.contend();
  ASSERT_SOME(candidacy);

  // Our detector should now "discover" the JSON, parse it
  // and derive the correct MasterInfo PB.
  ZooKeeperMasterDetector leaderDetector(url.get());
  Future<Option<MasterInfo>> detected = leaderDetector.detect(None());
  AWAIT_READY(detected);

  ASSERT_SOME_EQ(leader, detected.get());
}

#endif // MESOS_HAS_JAVA

} // namespace tests {
} // namespace internal {
} // namespace mesos {
