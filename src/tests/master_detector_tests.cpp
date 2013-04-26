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

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "master/master.hpp"

#include "messages/messages.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"
#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper_test.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;


class MasterDetectorTest : public MesosClusterTest {};


TEST_F(MasterDetectorTest, File)
{
  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  TestingIsolator isolator;
  Slave s(cluster.slaves.flags, true, &isolator, &cluster.files);
  PID<Slave> slave = process::spawn(&s);

  // Write "master" to a file and use the "file://" mechanism to
  // create a master detector for the slave. Still requires a master
  // detector for the master first.
  BasicMasterDetector detector1(master.get(), vector<UPID>(), true);

  const string& path = path::join(cluster.slaves.flags.work_dir, "master");
  ASSERT_SOME(os::write(path, stringify(master.get())));

  Try<MasterDetector*> detector =
    MasterDetector::create("file://" + path, slave, false, true);

  EXPECT_SOME(os::rm(path));

  ASSERT_SOME(detector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  driver.stop();
  driver.join();

  cluster.shutdown();

  process::terminate(slave);
  process::wait(slave);
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


#ifdef MESOS_HAS_JAVA
class ZooKeeperMasterDetectorTest : public ZooKeeperTest {};


TEST_F(ZooKeeperMasterDetectorTest, MasterDetector)
{
  MockMasterDetectorListenerProcess mock;
  process::spawn(mock);

  Future<Nothing> newMasterDetected;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(FutureSatisfy(&newMasterDetected));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector =
    MasterDetector::create(master, mock.self(), true, true);

  ASSERT_SOME(detector);

  AWAIT_READY(newMasterDetected);

  MasterDetector::destroy(detector.get());

  process::terminate(mock);
  process::wait(mock);
}


TEST_F(ZooKeeperMasterDetectorTest, MasterDetectors)
{
  MockMasterDetectorListenerProcess mock1;
  process::spawn(mock1);

  Future<Nothing> newMasterDetected1;
  EXPECT_CALL(mock1, newMasterDetected(mock1.self()))
    .WillOnce(FutureSatisfy(&newMasterDetected1));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector1 =
    MasterDetector::create(master, mock1.self(), true, true);

  ASSERT_SOME(detector1);

  AWAIT_READY(newMasterDetected1);

  MockMasterDetectorListenerProcess mock2;
  process::spawn(mock2);

  Future<Nothing> newMasterDetected2;
  EXPECT_CALL(mock2, newMasterDetected(mock1.self())) // N.B. mock1
    .WillOnce(FutureSatisfy(&newMasterDetected2));

  Try<MasterDetector*> detector2 =
    MasterDetector::create(master, mock2.self(), true, true);

  ASSERT_SOME(detector2);

  AWAIT_READY(newMasterDetected2);

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


TEST_F(ZooKeeperMasterDetectorTest, MasterDetectorShutdownNetwork)
{
  Clock::pause();

  MockMasterDetectorListenerProcess mock;
  process::spawn(mock);

  Future<Nothing> newMasterDetected1;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(FutureSatisfy(&newMasterDetected1));

  std::string master = "zk://" + server->connectString() + "/mesos";

  Try<MasterDetector*> detector =
    MasterDetector::create(master, mock.self(), true, true);

  ASSERT_SOME(detector);

  AWAIT_READY(newMasterDetected1);

  Future<Nothing> noMasterDetected;
  EXPECT_CALL(mock, noMasterDetected())
    .WillOnce(FutureSatisfy(&noMasterDetected));

  server->shutdownNetwork();

  Clock::advance(Seconds(10)); // TODO(benh): Get session timeout from detector.

  AWAIT_READY(noMasterDetected);

  Future<Nothing> newMasterDetected2;
  EXPECT_CALL(mock, newMasterDetected(mock.self()))
    .WillOnce(FutureSatisfy(&newMasterDetected2));

  server->startNetwork();

  AWAIT_READY(newMasterDetected2);

  MasterDetector::destroy(detector.get());

  process::terminate(mock);
  process::wait(mock);

  Clock::resume();
}


// Tests that a detector sends a NoMasterDetectedMessage when we
// reach our ZooKeeper session timeout. This is to enforce that we
// manually expire the session when we don't get reconnected within
// the ZOOKEEPER_SESSION_TIMEOUT.
TEST_F(ZooKeeperTest, MasterDetectorTimedoutSession)
{
  Try<zookeeper::URL> url =
    zookeeper::URL::parse("zk://" + server->connectString() + "/mesos");
  ASSERT_SOME(url);

  // First we bring up three master detector listeners:
  //   1. A leading contender.
  //   2. A non-leading contender.
  //   3. A non-contender.

  // 1. Simulate a leading contender.
  MockMasterDetectorListenerProcess leader;

  Future<Nothing> newMasterDetected;
  EXPECT_CALL(leader, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected));

  process::spawn(leader);

  ZooKeeperMasterDetector leaderDetector(
      url.get(), leader.self(), true, true);

  AWAIT_READY(newMasterDetected);

  // 2. Simulate a non-leading contender.
  MockMasterDetectorListenerProcess follower;

  EXPECT_CALL(follower, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected));

  process::spawn(follower);

  ZooKeeperMasterDetector followerDetector(
      url.get(), follower.self(), true, true);

  AWAIT_READY(newMasterDetected);

  // 3. Simulate a non-contender.
  MockMasterDetectorListenerProcess nonContender;

  EXPECT_CALL(nonContender, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected));

  process::spawn(nonContender);

  ZooKeeperMasterDetector nonContenderDetector(
      url.get(), nonContender.self(), false, true);

  AWAIT_READY(newMasterDetected);

  // Now we want to induce lost connections on each of the
  // master detectors.
  // Induce a reconnection on the leader.
  Future<Nothing> leaderReconnecting = FUTURE_DISPATCH(
      leaderDetector.process->self(),
      &ZooKeeperMasterDetectorProcess::reconnecting);

  dispatch(leaderDetector.process,
           &ZooKeeperMasterDetectorProcess::reconnecting);

  AWAIT_READY(leaderReconnecting);

  // Induce a reconnection on the follower.
  Future<Nothing> followerReconnecting = FUTURE_DISPATCH(
      followerDetector.process->self(),
      &ZooKeeperMasterDetectorProcess::reconnecting);

  dispatch(followerDetector.process,
           &ZooKeeperMasterDetectorProcess::reconnecting);

  AWAIT_READY(followerReconnecting);

  // Induce a reconnection on the non-contender.
  Future<Nothing> nonContenderReconnecting = FUTURE_DISPATCH(
      nonContenderDetector.process->self(),
      &ZooKeeperMasterDetectorProcess::reconnecting);

  dispatch(nonContenderDetector.process,
           &ZooKeeperMasterDetectorProcess::reconnecting);

  AWAIT_READY(nonContenderReconnecting);

  // Now induce the reconnection timeout.
  Future<Nothing> leaderNoMasterDetected;
  EXPECT_CALL(leader, noMasterDetected())
    .WillOnce(FutureSatisfy(&leaderNoMasterDetected));

  Future<Nothing> followerNoMasterDetected;
  EXPECT_CALL(follower, noMasterDetected())
    .WillOnce(FutureSatisfy(&followerNoMasterDetected));

  Future<Nothing> nonContenderNoMasterDetected;
  EXPECT_CALL(nonContender, noMasterDetected())
    .WillOnce(FutureSatisfy(&nonContenderNoMasterDetected));

  Clock::pause();
  Clock::advance(ZOOKEEPER_SESSION_TIMEOUT);
  Clock::settle();

  AWAIT_READY(leaderNoMasterDetected);
  AWAIT_READY(followerNoMasterDetected);
  AWAIT_READY(nonContenderNoMasterDetected);

  process::terminate(leader);
  process::wait(leader);

  process::terminate(follower);
  process::wait(follower);

  process::terminate(nonContender);
  process::wait(nonContender);
}


// Tests whether a leading master correctly detects a new master when
// its ZooKeeper session is expired.
TEST_F(ZooKeeperMasterDetectorTest, MasterDetectorExpireMasterZKSession)
{
  // Simulate a leading master.
  MockMasterDetectorListenerProcess leader;

  Future<Nothing> newMasterDetected1, newMasterDetected2;
  EXPECT_CALL(leader, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected1))
    .WillOnce(FutureSatisfy(&newMasterDetected2));

  EXPECT_CALL(leader, noMasterDetected())
    .Times(0);

  process::spawn(leader);

  std::string znode = "zk://" + server->connectString() + "/mesos";

  Try<zookeeper::URL> url = zookeeper::URL::parse(znode);
  ASSERT_SOME(url);

  // Leader's detector.
  ZooKeeperMasterDetector leaderDetector(
      url.get(), leader.self(), true, true);

  AWAIT_READY(newMasterDetected1);

  // Simulate a following master.
  MockMasterDetectorListenerProcess follower;

  Future<Nothing> newMasterDetected3;
  EXPECT_CALL(follower, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected3))
    .WillRepeatedly(Return());

  EXPECT_CALL(follower, noMasterDetected())
    .Times(0);

  process::spawn(follower);

  // Follower's detector.
  ZooKeeperMasterDetector followerDetector(
      url.get(),
      follower.self(),
      true,
      true);

  AWAIT_READY(newMasterDetected3);

  // Now expire the leader's zk session.
  Future<int64_t> session = leaderDetector.session();
  AWAIT_READY(session);

  server->expireSession(session.get());

  // Wait for session expiration and ensure we receive a
  // NewMasterDetected message.
  AWAIT_READY(newMasterDetected2);

  process::terminate(follower);
  process::wait(follower);

  process::terminate(leader);
  process::wait(leader);
}


// Tests whether a slave correctly DOES NOT disconnect from the master
// when its ZooKeeper session is expired, but the master still stays
// the leader when the slave re-connects with the ZooKeeper.
TEST_F(ZooKeeperMasterDetectorTest, MasterDetectorExpireSlaveZKSession)
{
  // Simulate a leading master.
  MockMasterDetectorListenerProcess master;

  Future<Nothing> newMasterDetected1;
  EXPECT_CALL(master, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected1));

  EXPECT_CALL(master, noMasterDetected())
    .Times(0);

  process::spawn(master);

  std::string znode = "zk://" + server->connectString() + "/mesos";

  Try<zookeeper::URL> url = zookeeper::URL::parse(znode);
  ASSERT_SOME(url);

  // Leading master's detector.
  ZooKeeperMasterDetector masterDetector(
      url.get(), master.self(), true, true);

  AWAIT_READY(newMasterDetected1);

  // Simulate a slave.
  MockMasterDetectorListenerProcess slave;

  Future<Nothing> newMasterDetected2, newMasterDetected3;
  EXPECT_CALL(slave, newMasterDetected(_))
    .Times(1)
    .WillOnce(FutureSatisfy(&newMasterDetected2));

  EXPECT_CALL(slave, noMasterDetected())
    .Times(0);

  process::spawn(slave);

  // Slave's master detector.
  ZooKeeperMasterDetector slaveDetector(
      url.get(), slave.self(), false, true);

  AWAIT_READY(newMasterDetected2);

  // Now expire the slave's zk session.
  Future<int64_t> session = slaveDetector.session();
  AWAIT_READY(session);

  server->expireSession(session.get());

  // Wait for enough time to ensure no NewMasterDetected message is sent.
  os::sleep(Seconds(4)); // ZooKeeper needs extra time for session expiration.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// Tests whether a slave correctly detects the new master when its
// ZooKeeper session is expired and a new master is elected before the
// slave reconnects with ZooKeeper.
TEST_F(ZooKeeperMasterDetectorTest, MasterDetectorExpireSlaveZKSessionNewMaster)
{
  // Simulate a leading master.
  MockMasterDetectorListenerProcess master1;

  Future<Nothing> newMasterDetected1;
  EXPECT_CALL(master1, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected1))
    .WillRepeatedly(Return());

  EXPECT_CALL(master1, noMasterDetected())
    .Times(0);

  process::spawn(master1);

  std::string znode = "zk://" + server->connectString() + "/mesos";

  Try<zookeeper::URL> url = zookeeper::URL::parse(znode);
  ASSERT_SOME(url);

  // Leading master's detector.
  ZooKeeperMasterDetector masterDetector1(
      url.get(), master1.self(), true, true);

  AWAIT_READY(newMasterDetected1);

  // Simulate a non-leading master.
  MockMasterDetectorListenerProcess master2;

  Future<Nothing> newMasterDetected2;
  EXPECT_CALL(master2, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected2))
    .WillRepeatedly(Return());

  EXPECT_CALL(master2, noMasterDetected())
    .Times(0);

  process::spawn(master2);

  // Non-leading master's detector.
  ZooKeeperMasterDetector masterDetector2(
      url.get(), master2.self(), true, true);

  AWAIT_READY(newMasterDetected2);

  // Simulate a slave.
  MockMasterDetectorListenerProcess slave;

  Future<Nothing> newMasterDetected3, newMasterDetected4;
  EXPECT_CALL(slave, newMasterDetected(_))
    .WillOnce(FutureSatisfy(&newMasterDetected3))
    .WillOnce(FutureSatisfy(&newMasterDetected4));

  EXPECT_CALL(slave, noMasterDetected())
    .Times(AtMost(1));

  process::spawn(slave);

  // Slave's master detector.
  ZooKeeperMasterDetector slaveDetector(
      url.get(), slave.self(), false, true);

  AWAIT_READY(newMasterDetected3);

  // Now expire the slave's and leading master's zk sessions.
  // NOTE: Here we assume that slave stays disconnected from the ZK when the
  // leading master loses its session.
  Future<int64_t> slaveSession = slaveDetector.session();
  AWAIT_READY(slaveSession);

  server->expireSession(slaveSession.get());

  Future<int64_t> masterSession = masterDetector1.session();
  AWAIT_READY(masterSession);

  server->expireSession(masterSession.get());

  // Wait for session expiration and ensure we receive a
  // NewMasterDetected message.
  AWAIT_READY(newMasterDetected4);

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master2);
  process::wait(master2);

  process::terminate(master1);
  process::wait(master1);
}
#endif // MESOS_HAS_JAVA
