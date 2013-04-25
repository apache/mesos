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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/message.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include "master/allocator.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/zookeeper.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::DoDefault;


template <typename T = AllocatorProcess>
class AllocatorZooKeeperTest : public MesosTest
{
public:
  static void SetUpTestCase()
  {
    // Make sure the JVM is created.
    ZooKeeperTest::SetUpTestCase();

    // Launch the ZooKeeper test server.
    server = new ZooKeeperTestServer();
    server->startNetwork();

    Try<zookeeper::URL> parse = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/znode");
    ASSERT_SOME(parse);

    url = parse.get();
  }

  static void TearDownTestCase()
  {
    delete server;
    server = NULL;
  }

protected:
  AllocatorZooKeeperTest() : MesosTest(url) {}

  static ZooKeeperTestServer* server;
  static Option<zookeeper::URL> url;
};


template <typename T>
ZooKeeperTestServer* AllocatorZooKeeperTest<T>::server = NULL;

template <typename T>
Option<zookeeper::URL> AllocatorZooKeeperTest<T>::url;


// Runs TYPED_TEST(AllocatorZooKeeperTest, ...) on all AllocatorTypes.
TYPED_TEST_CASE(AllocatorZooKeeperTest, AllocatorTypes);


// Checks that in the event of a master failure and the election of a
// new master, if a framework reregisters before a slave that it has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorZooKeeperTest, FrameworkReregistersFirst)
{
  TypeParam allocator1;

  Try<PID<Master> > master = this->StartMaster(&allocator1);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(this->url.get()));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer> > resourceOffers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(LaunchTasks(1, 1, 500),
                    FutureArg<1>(&resourceOffers1)))
    .WillRepeatedly(DeclineOffers());

  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, disconnected(_))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(resourceOffers1);

  // The framework will be offered all of the resources on the slave,
  // since it is the only framework running.
  EXPECT_THAT(resourceOffers1.get(), OfferEq(2, 1024));

  AWAIT_READY(statusUpdate);

  EXPECT_EQ(TASK_RUNNING, statusUpdate.get().state());

  // Stop the failing master from telling the slave to shut down when
  // it is killed.
  Future<ShutdownMessage> shutdownMessage =
    DROP_PROTOBUF(ShutdownMessage(), _, _);

  // Stop the slave from reregistering with the new master until the
  // framework has reregistered.
  DROP_PROTOBUFS(ReregisterSlaveMessage(), _, _);

  this->ShutdownMasters();

  AWAIT_READY(shutdownMessage);

  MockAllocatorProcess<TypeParam> allocator2;

  EXPECT_CALL(allocator2, initialize(_, _));

  Try<PID<Master> > master2 = this->StartMaster(&allocator2);
  ASSERT_SOME(master2);

  Future<Nothing> frameworkAdded;
  EXPECT_CALL(allocator2, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator2),
                    FutureSatisfy(&frameworkAdded)));

  EXPECT_CALL(sched, reregistered(&driver, _));

  AWAIT_READY(frameworkAdded);

  EXPECT_CALL(allocator2, slaveAdded(_, _, _));

  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  // We kill the filter so that ReregisterSlaveMessages can get
  // to the master now that the framework has been added, ensuring
  // that the slave reregisters after the framework.
  process::filter(NULL);

  AWAIT_READY(resourceOffers2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

  // Shut everything down.
  EXPECT_CALL(allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator2, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(allocator2, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(allocator2, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}


// Checks that in the event of a master failure and the election of a
// new master, if a slave reregisters before a framework that has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(AllocatorZooKeeperTest, SlaveReregistersFirst)
{
  TypeParam allocator1;

  Try<PID<Master> > master = this->StartMaster(&allocator1);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  Try<PID<Slave> > slave = this->StartSlave(&exec, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(this->url.get()));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer> > resourceOffers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(LaunchTasks(1, 1, 500),
                    FutureArg<1>(&resourceOffers1)))
    .WillRepeatedly(DeclineOffers());

  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, disconnected(_))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(resourceOffers1);

  // The framework will be offered all of the resources on the slave,
  // since it is the only framework running.
  EXPECT_THAT(resourceOffers1.get(), OfferEq(2, 1024));

  AWAIT_READY(statusUpdate);

  EXPECT_EQ(TASK_RUNNING, statusUpdate.get().state());

  // Stop the failing master from telling the slave to shut down when
  // it is killed.
  Future<ShutdownMessage> shutdownMessage =
    DROP_PROTOBUF(ShutdownMessage(), _, _);

  // Stop the framework from reregistering with the new master until the
  // slave has reregistered.
  DROP_PROTOBUFS(ReregisterFrameworkMessage(), _, _);

  this->ShutdownMasters();

  AWAIT_READY(shutdownMessage);

  MockAllocatorProcess<TypeParam> allocator2;

  EXPECT_CALL(allocator2, initialize(_, _));

  Try<PID<Master> > master2 = this->StartMaster(&allocator2);
  ASSERT_SOME(master2);

  Future<Nothing> slaveAdded;
  EXPECT_CALL(allocator2, slaveAdded(_, _, _))
    .WillOnce(DoAll(InvokeSlaveAdded(&allocator2),
                    FutureSatisfy(&slaveAdded)));

  EXPECT_CALL(sched, reregistered(&driver, _));

  AWAIT_READY(slaveAdded);

  EXPECT_CALL(allocator2, frameworkAdded(_, _, _));

  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  // We kill the filter so that ReregisterFrameworkMessages can get
  // to the master now that the framework has been added, ensuring
  // that the framework reregisters after the slave.
  process::filter(NULL);

  AWAIT_READY(resourceOffers2);

  // Since the task is still running on the slave, the framework
  // should only be offered the resources not being used by the task.
  EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

  // Shut everything down.
  EXPECT_CALL(allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator2, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(allocator2, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(allocator2, slaveRemoved(_))
    .Times(AtMost(1));

  this->Shutdown();
}
