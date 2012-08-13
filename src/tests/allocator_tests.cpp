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

#include <mesos/scheduler.hpp>

#include "configurator/configuration.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/dominant_share_allocator.hpp"
#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"

#include "tests/base_zookeeper_test.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Allocator;
using mesos::internal::master::DominantShareAllocator;
using mesos::internal::master::Master;

using mesos::internal::slave::ProcessBasedIsolationModule;
using mesos::internal::slave::Slave;

using process::PID;

using std::string;
using std::vector;

using testing::AnyNumber;
using testing::ByRef;
using testing::DoDefault;
using testing::Eq;
using testing::_;
using testing::Return;
using testing::SaveArg;

void checkResources(vector<Offer> offers, int cpus, int mem)
{
  EXPECT_EQ(offers.size(), 1);

  EXPECT_EQ(offers[0].resources().size(), 2);

  foreach (const Resource& resource, offers[0].resources()) {
    if (resource.name() == "cpus") {
      EXPECT_EQ(resource.scalar().value(), cpus);
    } else if (resource.name() == "mem") {
      EXPECT_EQ(resource.scalar().value(), mem);
    }
  }
}


TEST(AllocatorTest, DominantShareAllocator)
{
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  FrameworkID frameworkId1;

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  FrameworkID frameworkId2;

  FrameworkInfo frameworkInfo3;
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_user("user1");
  FrameworkID frameworkId3;

  MockAllocator<DominantShareAllocator> a;

  EXPECT_CALL(a, initialize(_, _));

  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId1)));

  trigger framework2Added;
  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId2),
		    Trigger(&framework2Added)));

  trigger framework3Added;
  EXPECT_CALL(a, frameworkAdded(_, Eq(frameworkInfo3), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&a),
		    SaveArg<0>(&frameworkId3),
		    Trigger(&framework3Added)));

  EXPECT_CALL(a, frameworkDeactivated(_))
    .Times(3);

  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId1))));

  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId2))));

  trigger lastFrameworkRemoved;
  EXPECT_CALL(a, frameworkRemoved(Eq(ByRef(frameworkId3))))
    .WillOnce(Trigger(&lastFrameworkRemoved));

  SlaveID slaveId4;
  EXPECT_CALL(a, slaveAdded(_, _, _))
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoDefault())
    .WillOnce(DoAll(InvokeSlaveAdded(&a),
		    SaveArg<0>(&slaveId4)));

  EXPECT_CALL(a, slaveRemoved(_))
    .Times(3);

  trigger lastSlaveRemoved;
  EXPECT_CALL(a, slaveRemoved(Eq(ByRef(slaveId4))))
    .WillOnce(Trigger(&lastSlaveRemoved));

  EXPECT_CALL(a, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  Master m(&a);
  PID<Master> master = process::spawn(m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources = Resources::parse("cpus:2;mem:1024");
  Slave s(resources, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s);

  BasicMasterDetector detector(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  vector<Offer> offers1;
  trigger resourceOfferTrigger;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers1),
		    Trigger(&resourceOfferTrigger)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(resourceOfferTrigger);

  checkResources(offers1, 2, 1024);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  vector<Offer> offers2, offers3;
  trigger resourceOfferTrigger2, resourceOfferTrigger3;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
		    Trigger(&resourceOfferTrigger2)))
    .WillOnce(DoAll(SaveArg<1>(&offers3),
		    Trigger(&resourceOfferTrigger3)))
    .WillRepeatedly(Return());

  driver2.start();

  WAIT_UNTIL(framework2Added);

  Resources resources2 = Resources::parse("cpus:1;mem:512");
  Slave s2(resources2, true, &isolationModule);
  PID<Slave> slave2 = process::spawn(s2);

  BasicMasterDetector detector2(master, slave2, true);

  WAIT_UNTIL(resourceOfferTrigger2);

  checkResources(offers2, 1, 512);

  Resources resources3 = Resources::parse("cpus:3;mem:2048");
  Slave s3(resources3, true, &isolationModule);
  PID<Slave> slave3 = process::spawn(s3);
  BasicMasterDetector detector3(master, slave3, true);

  WAIT_UNTIL(resourceOfferTrigger3);

  checkResources(offers3, 3, 2048);

  MockScheduler sched3;
  MesosSchedulerDriver driver3(&sched3, frameworkInfo3, master);

  EXPECT_CALL(sched3, registered(_, _, _));

  vector<Offer> offers4;
  trigger resourceOfferTrigger4;
  EXPECT_CALL(sched3, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers4),
		    Trigger(&resourceOfferTrigger4)));

  driver3.start();

  WAIT_UNTIL(framework3Added);

  Resources resources4 = Resources::parse("cpus:4;mem:4096");
  Slave s4(resources4, true, &isolationModule);
  PID<Slave> slave4 = process::spawn(s4);
  BasicMasterDetector detector4(master, slave4, true);

  WAIT_UNTIL(resourceOfferTrigger4);

  checkResources(offers4, 4, 4096);

  driver3.stop();
  driver2.stop();
  driver1.stop();

  WAIT_UNTIL(lastFrameworkRemoved);

  process::terminate(slave1);
  process::wait(slave1);

  process::terminate(slave2);
  process::wait(slave2);

  process::terminate(slave3);
  process::wait(slave3);

  process::terminate(slave4);
  process::wait(slave4);

  WAIT_UNTIL(lastSlaveRemoved);

  process::terminate(master);
  process::wait(master);
}


template <typename T = Allocator>
class AllocatorTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    process::spawn(allocator.real);
  }

  virtual void TearDown()
  {
    process::terminate(allocator.real);
    process::wait(allocator.real);
  }

  MockAllocator<T> allocator;
};


// Causes all TYPED_TEST(AllocatorTest, ...) to be run for
// each of the specified Allocator classes.
typedef ::testing::Types<DominantShareAllocator> AllocatorTypes;
TYPED_TEST_CASE(AllocatorTest, AllocatorTypes);


TYPED_TEST(AllocatorTest, MockAllocator)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  Master m(&this->allocator);
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffers;

  vector<Offer> offers;

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&resourceOffers)));

  driver.start();

  WAIT_UNTIL(resourceOffers);

  checkResources(offers, 2, 1024);

  driver.stop();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


TYPED_TEST(AllocatorTest, ResourcesUnused)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _))
    .Times(2);

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  trigger resourcesUnusedTrigger;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
		    Trigger(&resourcesUnusedTrigger),
		    Return()))
    .WillRepeatedly(DoDefault());

  // Prevent any resources from being recovered by returning instead
  // of allowing resourcesRecovered to be invoked, so the only ones
  // that are reallocated are those returned by resourcesUnused.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(Return());

  Master m(&(this->allocator));
  PID<Master> master = process::spawn(m);

  ProcessBasedIsolationModule isolationModule;
  Resources resources1 = Resources::parse("cpus:2;mem:1024");
  Slave s(resources1, true, &isolationModule);
  PID<Slave> slave1 = process::spawn(s);
  BasicMasterDetector(master, slave1, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(LaunchTasks(1, 1, 512))
    .WillRepeatedly(DeclineOffers());

  driver1.start();

  WAIT_UNTIL(resourcesUnusedTrigger);

  FrameworkInfo info2;
  info2.set_user("user2");
  info2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, info2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  vector<Offer> offers;
  trigger offered;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&offered)))
    .WillRepeatedly(Return());

  driver2.start();

  WAIT_UNTIL(offered);

  checkResources(offers, 1, 512);

  driver1.stop();
  driver2.stop();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave1);
  process::wait(slave1);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


// Tests the situation where a frameworkRemoved call is dispatched
// while we're doing an allocation to that framework, so that
// resourcesRecovered is called for an already removed framework.
TYPED_TEST(AllocatorTest, OutOfOrderDispatch)
{
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  FrameworkID frameworkId1;

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  FrameworkID frameworkId2;

  EXPECT_CALL(this->allocator, initialize(_, _));

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
		    SaveArg<0>(&frameworkId1)));

  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
		    SaveArg<0>(&frameworkId2)));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  trigger frameworkRemoved, frameworkRemoved2;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(ByRef(frameworkId1))))
    .WillOnce(DoAll(InvokeFrameworkRemoved(&this->allocator),
		    Trigger(&frameworkRemoved),
		    Return()));

  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(ByRef(frameworkId2))))
    .WillOnce(Trigger(&frameworkRemoved2));

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources savedResources;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    // "Catches" the resourcesRecovered call from the master, so
    // that it doesn't get processed until we redispatch it after
    // the frameworkRemoved trigger.
    .WillOnce(DoAll(SaveArg<0>(&frameworkId),
		    SaveArg<1>(&slaveId),
		    SaveArg<2>(&savedResources)))
    .WillRepeatedly(DoDefault());

  Master m(&(this->allocator));
  PID<Master> master = process::spawn(&m);

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, frameworkInfo1, master);

  trigger offered;
  vector<Offer> offers;

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&offered)));

  driver.start();

  WAIT_UNTIL(offered);

  checkResources(offers, 2, 1024);

  driver.stop();
  driver.join();

  WAIT_UNTIL(frameworkRemoved);

  // Re-dispatch the resourcesRecovered call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  dispatch(this->allocator, &Allocator::resourcesRecovered,
	   frameworkId, slaveId, savedResources);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master);

  EXPECT_CALL(sched2, registered(_, _, _));

  trigger offered2;
  vector<Offer> offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
		    Trigger(&offered2)));

  driver2.start();

  WAIT_UNTIL(offered2);

  checkResources(offers2, 2, 1024);

  driver2.stop();
  driver2.join();

  WAIT_UNTIL(frameworkRemoved2);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master);
  process::wait(master);
}


template <typename T = Allocator>
class MasterFailoverAllocatorTest : public BaseZooKeeperTest
{
public:
  static void SetUpTestCase() {
    BaseZooKeeperTest::SetUpTestCase();
  }

protected:
  T allocator1;
  MockAllocator<T> allocator2;
};


// Runs TYPED_TEST(MasterFailoverAllocatorTest, ...) on all AllocatorTypes.
TYPED_TEST_CASE(MasterFailoverAllocatorTest, AllocatorTypes);


TYPED_TEST(MasterFailoverAllocatorTest, MasterFailover)
{
  trigger slaveAdded;
  EXPECT_CALL(this->allocator2, initialize(_, _));

  trigger frameworkAdded;
  EXPECT_CALL(this->allocator2, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator2, frameworkDeactivated(_));

  trigger frameworkRemovedTrigger;
  EXPECT_CALL(this->allocator2, frameworkRemoved(_))
    .WillOnce(Trigger(&frameworkRemovedTrigger));

  EXPECT_CALL(this->allocator2, slaveAdded(_, _, _));

  trigger slaveRemovedTrigger;
  EXPECT_CALL(this->allocator2, slaveRemoved(_))
    .WillOnce(Trigger(&slaveRemovedTrigger));

  EXPECT_CALL(this->allocator2, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, Eq(ShutdownMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  Master m1(&(this->allocator1));
  PID<Master> master1 = process::spawn(m1);

  string zk = "zk://" + this->zks->connectString() + "/znode";
  Try<MasterDetector*> detector =
    MasterDetector::create(zk, master1, true, true);
  CHECK(!detector.isError())
    << "Failed to create a master detector: " << detector.error();

  ProcessBasedIsolationModule isolationModule;

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(s);

  Try<MasterDetector*> slave_detector =
    MasterDetector::create(zk, slave, false, true);
  CHECK(!slave_detector.isError())
    << "Failed to create a master detector: " << slave_detector.error();

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, zk);

  trigger resourceOffers, resourceOffers2;

  vector<Offer> offers, offers2;

  EXPECT_CALL(sched, registered(_, _, _))
    .Times(2);

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
		    Trigger(&resourceOffers)))
    .WillOnce(DoAll(SaveArg<1>(&offers2),
		    Trigger(&resourceOffers2)));

  EXPECT_CALL(sched, disconnected(_))
    .WillRepeatedly(DoDefault());

  driver.start();

  WAIT_UNTIL(resourceOffers);

  checkResources(offers, 2, 1024);

  process::terminate(master1);
  process::wait(master1);
  MasterDetector::destroy(detector.get());

  Master m2(&(this->allocator2));
  PID<Master> master2 = process::spawn(m2);

  Try<MasterDetector*> detector2 =
    MasterDetector::create(zk, master2, true, true);
  CHECK(!detector2.isError())
    << "Failed to create a master detector: " << detector2.error();

  WAIT_UNTIL(resourceOffers2);

  checkResources(offers2, 2, 1024);

  driver.stop();

  WAIT_UNTIL(frameworkRemovedTrigger);

  process::terminate(slave);
  process::wait(slave);

  WAIT_UNTIL(slaveRemovedTrigger);

  process::terminate(master2);
  process::wait(master2);

  process::filter(NULL);
}
