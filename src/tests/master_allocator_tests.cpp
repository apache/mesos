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

#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/module/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/queue.hpp>

#include <stout/some.hpp>
#include <stout/strings.hpp>

#include "master/constants.hpp"
#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/detector/standalone.hpp"

#include "tests/allocator.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"
#include "tests/resources_utils.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::allocator::Allocator;
using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Queue;

using process::http::OK;
using process::http::Response;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class OfferEqMatcher
  : public ::testing::MatcherInterface<const vector<Offer>&>
{
public:
  OfferEqMatcher(int _cpus, int _mem)
    : cpus(_cpus), mem(_mem) {}

  virtual bool MatchAndExplain(
      const vector<Offer>& offers,
      ::testing::MatchResultListener* listener) const
  {
    double totalCpus = 0;
    double totalMem = 0;

    foreach (const Offer& offer, offers) {
      foreach (const Resource& resource, offer.resources()) {
        if (resource.name() == "cpus") {
          totalCpus += resource.scalar().value();
        } else if (resource.name() == "mem") {
          totalMem += resource.scalar().value();
        }
      }
    }

    bool matches = totalCpus == cpus && totalMem == mem;

    if (!matches) {
      *listener << totalCpus << " cpus and " << totalMem << "mem";
    }

    return matches;
  }

  virtual void DescribeTo(::std::ostream* os) const
  {
    *os << "contains " << cpus << " cpus and " << mem << " mem";
  }

  virtual void DescribeNegationTo(::std::ostream* os) const
  {
    *os << "does not contain " << cpus << " cpus and "  << mem << " mem";
  }

private:
  int cpus;
  int mem;
};


const ::testing::Matcher<const vector<Offer>&> OfferEq(int cpus, int mem)
{
  return MakeMatcher(new OfferEqMatcher(cpus, mem));
}


template <typename T>
class MasterAllocatorTest : public MesosTest {};

typedef ::testing::Types<HierarchicalDRFAllocator,
                         tests::Module<Allocator, TestDRFAllocator>>
  AllocatorTypes;


// Causes all TYPED_TEST(MasterAllocatorTest, ...) to be run for
// each of the specified Allocator classes.
TYPED_TEST_CASE(MasterAllocatorTest, AllocatorTypes);


#ifndef __WINDOWS__
// Checks that in a cluster with one slave and one framework, all of
// the slave's resources are offered to the framework.
TYPED_TEST(MasterAllocatorTest, SingleFramework)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:1024;disk:0");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = this->StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // The framework should be offered all of the resources on the slave
  // since it is the only framework in the cluster.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// Checks that when a task is launched with fewer resources than what
// the offer was for, the resources that are returned unused are
// reoffered appropriately.
TYPED_TEST(MasterAllocatorTest, ResourcesUnused)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 =
    this->StartSlave(detector.get(), &containerizer, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first offer will contain all of the slave's resources, since
  // this is the only framework running so far. Launch a task that
  // uses less than that to leave some resources unused.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"));

  Future<Nothing> recoverResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverResources)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  AWAIT_READY(launchTask);

  // We need to wait until the allocator knows about the unused
  // resources to start the second framework so that we get the
  // expected offer.
  AWAIT_READY(recoverResources);

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // We should expect that framework2 gets offered all of the
  // resources on the slave not being used by the launched task.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  // Shut everything down.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.
}


// Tests the situation where a removeFramework call is dispatched
// while we're doing an allocation to that framework, so that
// recoverResources is called for an already removed framework.
TYPED_TEST(MasterAllocatorTest, OutOfOrderDispatch)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 = this->StartSlave(detector.get(), flags1);
  ASSERT_SOME(slave1);

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, Eq(frameworkInfo1), _, _))
    .WillOnce(InvokeAddFramework(&allocator));

  Future<FrameworkID> frameworkId1;
  EXPECT_CALL(sched1, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId1));

  // All of the slave's resources should be offered to start.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver1.start();

  AWAIT_READY(frameworkId1);
  AWAIT_READY(resourceOffers);

  // TODO(benh): I don't see why we want to "catch" (i.e., block) this
  // recoverResources call. It seems like we want this one to
  // properly be executed and later we want to _inject_ a
  // recoverResources to simulate the code in Master::offer after a
  // framework has terminated or is inactive.
  Future<SlaveID> slaveId;
  Future<Resources> savedResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    // "Catches" the recoverResources call from the master, so
    // that it doesn't get processed until we redispatch it after
    // the removeFramework trigger.
    .WillOnce(DoAll(FutureArg<1>(&slaveId),
                    FutureArg<2>(&savedResources)));

  EXPECT_CALL(allocator, deactivateFramework(_));

  Future<Nothing> removeFramework;
  EXPECT_CALL(allocator, removeFramework(Eq(frameworkId1.get())))
    .WillOnce(DoAll(InvokeRemoveFramework(&allocator),
                    FutureSatisfy(&removeFramework)));

  driver1.stop();
  driver1.join();

  AWAIT_READY(removeFramework);
  AWAIT_READY(slaveId);
  AWAIT_READY(savedResources);

  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoDefault()); // For the re-dispatch.

  // Re-dispatch the recoverResources call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  allocator.recoverResources(
      frameworkId1.get(),
      slaveId.get(),
      savedResources.get(),
      None());

  // TODO(benh): Seems like we should wait for the above
  // recoverResources to be executed.

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, Eq(frameworkInfo2), _, _))
    .WillOnce(InvokeAddFramework(&allocator));

  FrameworkID frameworkId2;
  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId2));

  // All of the slave's resources should be offered since no other
  // frameworks should be running.
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  // Called when driver2 stops.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());
  EXPECT_CALL(allocator, deactivateFramework(_))
    .WillRepeatedly(DoDefault());
  EXPECT_CALL(allocator, removeFramework(_))
    .WillRepeatedly(DoDefault());

  // Shut everything down.
  driver2.stop();
  driver2.join();
}


// Checks that if a framework launches a task and then fails over to a
// new scheduler, the task's resources are not reoffered as long as it
// is running.
TYPED_TEST(MasterAllocatorTest, SchedulerFailover)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_failover_timeout(10);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers()); // For subsequent offers.

  // Initially, all of slave1's resources are available.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 256, "*"));

  // We don't filter the unused resources to make sure that
  // they get offered to the framework as soon as it fails over.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(InvokeRecoverResourcesWithFilters(&allocator, 0))
    // For subsequent offers.
    .WillRepeatedly(InvokeRecoverResourcesWithFilters(&allocator, 0));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  // Ensures that the task has been completely launched
  // before we have the framework fail over.
  AWAIT_READY(launchTask);

  // When we shut down the first framework, we don't want it to tell
  // the master it's shutting down so that the master will wait to see
  // if it fails over.
  DROP_CALLS(mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  Future<Nothing> deactivateFramework;
  EXPECT_CALL(allocator, deactivateFramework(_))
    .WillOnce(DoAll(InvokeDeactivateFramework(&allocator),
                    FutureSatisfy(&deactivateFramework)));

  driver1.stop();

  AWAIT_READY(deactivateFramework);

  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.mutable_id()->MergeFrom(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, activateFramework(_));

  EXPECT_CALL(sched2, registered(_, frameworkId, _));

  // Even though the scheduler failed over, the 1 cpu, 256 mem
  // task that it launched earlier should still be running, so
  // only 2 cpus and 768 mem are available.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver2.start();

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Shut everything down.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, deactivateFramework(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();
}


// Checks that if a framework launches a task and then the framework
// is killed, the tasks resources are returned and reoffered correctly.
TYPED_TEST(MasterAllocatorTest, FrameworkExited)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  ExecutorInfo executor1 = createExecutorInfo("executor-1", "exit 1");
  ExecutorInfo executor2 = createExecutorInfo("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time the framework is offered resources, all of the
  // cluster's resources should be available.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(executor1, 1, 2, 512, "*"));

  // The framework does not use all the resources.
  Future<Nothing> recoverResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverResources)));

  EXPECT_CALL(exec1, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  // Ensures that framework 1's task is completely launched
  // before we kill the framework to test if its resources
  // are recovered correctly.
  AWAIT_READY(launchTask);

  // We need to wait until the allocator knows about the unused
  // resources to start the second framework so that we get the
  // expected offer.
  AWAIT_READY(recoverResources);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time sched2 gets an offer, framework 1 has a task
  // running with 2 cpus and 512 mem, leaving 1 cpu and 512 mem.
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(LaunchTasks(executor2, 1, 1, 256, "*"));

  // The framework 2 does not use all the resources.
  Future<Nothing> recoverResources2;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverResources2)));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver2.start();

  AWAIT_READY(launchTask);

  AWAIT_READY(recoverResources2);

  // Shut everything down but check that framework 2 gets the
  // resources from framework 1 after it is shutdown.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  // After we stop framework 1, all of its resources should
  // have been returned, but framework 2 should still have a
  // task with 1 cpu and 256 mem, leaving 2 cpus and 768 mem.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();
}


// Checks that if a framework launches a task and then the slave the
// task was running on gets killed, the task's resources are properly
// recovered and, along with the rest of the resources from the killed
// slave, never offered again.
TYPED_TEST(MasterAllocatorTest, SlaveLost)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags1 = this->CreateSlaveFlags();

  // Set the `executor_shutdown_grace_period` to a small value so that
  // the agent does not wait for executors to clean up for too long.
  flags1.executor_shutdown_grace_period = Milliseconds(50);
  flags1.resources = Some("cpus:2;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 =
    this->StartSlave(detector.get(), &containerizer, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Initially, all of slave1's resources are available.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 512, "*"));

  Future<Nothing> recoverResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(InvokeRecoverResources(&allocator),
                    FutureSatisfy(&recoverResources)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  // Ensures the task is completely launched before we kill the
  // slave, to test that the task's and executor's resources are
  // recovered correctly (i.e. never reallocated since the slave
  // is killed).
  AWAIT_READY(launchTask);

  // Framework does not use all the resources.
  AWAIT_READY(recoverResources);

  // 'recoverResources' should be called twice, once for the task
  // and once for the executor.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .Times(2);

  Future<Nothing> removeSlave;
  EXPECT_CALL(allocator, removeSlave(_))
    .WillOnce(DoAll(InvokeRemoveSlave(&allocator),
                    FutureSatisfy(&removeSlave)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(_, _));

  // Stop the checkpointing slave with explicit shutdown message
  // so that the master does not wait for it to reconnect.
  slave1.get()->shutdown();

  AWAIT_READY(removeSlave);

  slave::Flags flags2 = this->CreateSlaveFlags();
  flags2.resources = string("cpus:3;gpus:0;mem:256;"
                            "disk:1024;ports:[31000-32000]");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  // Eventually after slave2 is launched, we should get
  // an offer that contains all of slave2's resources
  // and none of slave1's resources.
  Future<vector<Offer>> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 256)))
    .WillOnce(FutureArg<1>(&resourceOffers));

  Try<Owned<cluster::Slave>> slave2 = this->StartSlave(detector.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers);

  EXPECT_EQ(Resources(resourceOffers.get()[0].resources()),
            allocatedResources(
                Resources::parse(flags2.resources.get()).get(),
                DEFAULT_FRAMEWORK_INFO.role()));

  // Shut everything down.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();

  EXPECT_CALL(allocator, removeSlave(_))
    .Times(AtMost(1));
}


// Checks that if a slave is added after some allocations have already
// occurred, its resources are added to the available pool of
// resources and offered appropriately.
TYPED_TEST(MasterAllocatorTest, SlaveAdded)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags1 = this->CreateSlaveFlags();
  flags1.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 =
    this->StartSlave(detector.get(), &containerizer, flags1);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of slave1's resources are available.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 512, "*"));

  // We filter the first time so that the unused resources
  // on slave1 from the task launch won't get reoffered
  // immediately and will get combined with slave2's
  // resources for a single offer.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(InvokeRecoverResourcesWithFilters(&allocator, 0.1))
    .WillRepeatedly(InvokeRecoverResourcesWithFilters(&allocator, 0));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  slave::Flags flags2 = this->CreateSlaveFlags();
  flags2.resources = Some("cpus:4;mem:2048");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  // After slave2 launches, all of its resources are combined with the
  // resources on slave1 that the task isn't using.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(5, 2560)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Try<Owned<cluster::Slave>> slave2 = this->StartSlave(detector.get(), flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Shut everything down.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();
}


// Checks that if a task is launched and then finishes normally, its
// resources are recovered and reoffered correctly.
TYPED_TEST(MasterAllocatorTest, TaskFinished)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:3;mem:1024");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // We decline offers that we aren't expecting so that the resources
  // get aggregated. Note that we need to do this _first_ and
  // _separate_ from the expectation below so that this expectation is
  // checked last and matches all possible offers.
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of the slave's resources.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 2, 1, 256, "*"));

  // Some resources will be unused and we need to make sure that we
  // don't send the TASK_FINISHED status update below until after the
  // allocator knows about the unused resources so that it can
  // aggregate them with the resources from the finished task.
  Future<Nothing> recoverResources;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoAll(InvokeRecoverResources(&allocator),
                          FutureSatisfy(&recoverResources)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  AWAIT_READY(recoverResources);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  EXPECT_CALL(allocator, recoverResources(_, _, _, _));

  // After the first task gets killed.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Shut everything down.
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillRepeatedly(DoDefault());

  driver.stop();
  driver.join();
}


// Checks that cpus only resources are offered
// and tasks using only cpus are launched.
TYPED_TEST(MasterAllocatorTest, CpusOnlyOfferedAndTaskLaunched)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Start a slave with cpus only resources.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:2;mem:0");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Launch a cpus only task.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 0)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 0, "*"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  // Check that cpus resources of finished task are offered again.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 0)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Shut everything down.
  driver.stop();
  driver.join();
}


// Checks that memory only resources are offered
// and tasks using only memory are launched.
TYPED_TEST(MasterAllocatorTest, MemoryOnlyOfferedAndTaskLaunched)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Start a slave with memory only resources.
  slave::Flags flags = this->CreateSlaveFlags();
  flags.resources = Some("cpus:0;mem:200");

  EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Launch a memory only task.
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(0, 200)))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 0, 200, "*"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  driver.start();

  AWAIT_READY(launchTask);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  // Check that mem resources of finished task are offered again.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(0, 200)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Shut everything down.
  driver.stop();
  driver.join();
}


// Checks that changes to the whitelist are sent to the allocator.
// The allocator whitelisting is tested in the allocator unit tests.
// TODO(bmahler): Move this to a whitelist unit test.
TYPED_TEST(MasterAllocatorTest, Whitelist)
{
  Clock::pause();

  // Create a dummy whitelist, so that no resources will get allocated.
  hashset<string> hosts;
  hosts.insert("dummy-agent1");

  const string path = "whitelist.txt";
  ASSERT_SOME(os::write(path, strings::join("\n", hosts)));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.whitelist = path;

  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  Future<Nothing> updateWhitelist1;
  EXPECT_CALL(allocator, updateWhitelist(Option<hashset<string>>(hosts)))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&allocator),
                    FutureSatisfy(&updateWhitelist1)));

  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Make sure the allocator has been given the initial whitelist.
  AWAIT_READY(updateWhitelist1);

  // Update the whitelist to ensure that the change is sent
  // to the allocator.
  hosts.insert("dummy-agent2");

  Future<Nothing> updateWhitelist2;
  EXPECT_CALL(allocator, updateWhitelist(Option<hashset<string>>(hosts)))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&allocator),
                    FutureSatisfy(&updateWhitelist2)));

  ASSERT_SOME(os::write(path, strings::join("\n", hosts)));

  Clock::advance(mesos::internal::master::WHITELIST_WATCH_INTERVAL);

  // Make sure the allocator has been given the updated whitelist.
  AWAIT_READY(updateWhitelist2);
}


// Checks that a framework attempting to register with an invalid role
// will receive an error message and that roles can be added through the
// master's command line flags.
TYPED_TEST(MasterAllocatorTest, RoleTest)
{
  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  master::Flags masterFlags = this->CreateMasterFlags();
  masterFlags.roles = Some("role2");
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  // Launch a framework with a role that doesn't exist to see that it
  // receives an error message.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_role("role1");

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkErrorMessage> errorMessage =
    FUTURE_PROTOBUF(FrameworkErrorMessage(), _, _);

  EXPECT_CALL(sched1, error(_, _));

  driver1.start();

  AWAIT_READY(errorMessage);

  // Launch a framework under an existing role to see that it registers.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_role("role2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  Future<Nothing> addFramework;
  EXPECT_CALL(allocator, addFramework(_, _, _, _))
    .WillOnce(FutureSatisfy(&addFramework));

  driver2.start();

  AWAIT_READY(registered2);
  AWAIT_READY(addFramework);

  // Shut everything down.
  Future<Nothing> deactivateFramework;
  EXPECT_CALL(allocator, deactivateFramework(_))
    .WillOnce(FutureSatisfy(&deactivateFramework));

  Future<Nothing> removeFramework;
  EXPECT_CALL(allocator, removeFramework(_))
    .WillOnce(FutureSatisfy(&removeFramework));

  driver2.stop();
  driver2.join();

  AWAIT_READY(deactivateFramework);
  AWAIT_READY(removeFramework);

  driver1.stop();
  driver1.join();
}


// Checks that in the event of a master failure and the election of a
// new master, if a framework reregisters before a slave that it has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(MasterAllocatorTest, FrameworkReregistersFirst)
{
  // Objects that persist after the election of a new master.
  StandaloneMasterDetector slaveDetector;
  StandaloneMasterDetector schedulerDetector;
  MockScheduler sched;
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);
  Try<Owned<cluster::Slave>> slave = nullptr;

  // Explicit scope is to ensure all object associated with the
  // leading master (e.g. allocator) are destroyed once the master
  // is shut down. Otherwise subsequent (not expected) calls into
  // Allocator::recoverResources() are possibly rendering tests flaky.
  {
    TestAllocator<TypeParam> allocator;

    EXPECT_CALL(allocator, initialize(_, _, _, _));

    Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
    ASSERT_SOME(master);

    slaveDetector.appoint(master.get()->pid);
    schedulerDetector.appoint(master.get()->pid);

    EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

    slave::Flags flags = this->CreateSlaveFlags();
    flags.resources = Some("cpus:2;mem:1024");

    slave = this->StartSlave(&slaveDetector, &containerizer, flags);
    ASSERT_SOME(slave);

    EXPECT_CALL(allocator, addFramework(_, _, _, _));

    EXPECT_CALL(sched, registered(&driver, _, _));

    // The framework should be offered all of the resources on the
    // slave since it is the only framework running.
    EXPECT_CALL(sched, resourceOffers(&driver, OfferEq(2, 1024)))
      .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 500, "*"))
      .WillRepeatedly(DeclineOffers());

    EXPECT_CALL(allocator, recoverResources(_, _, _, _));

    EXPECT_CALL(exec, registered(_, _, _, _));

    EXPECT_CALL(exec, launchTask(_, _))
      .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

    Future<TaskStatus> status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&status));

    Future<Nothing> _statusUpdateAcknowledgement =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

    driver.start();

    AWAIT_READY(status);

    EXPECT_EQ(TASK_RUNNING, status->state());

    // Make sure the slave handles status update acknowledgement so
    // that it doesn't try to retry the update after master failover.
    AWAIT_READY(_statusUpdateAcknowledgement);

    EXPECT_CALL(allocator, recoverResources(_, _, _, _))
      .WillRepeatedly(DoDefault());
  }

  {
    TestAllocator<TypeParam> allocator2;

    EXPECT_CALL(allocator2, initialize(_, _, _, _));

    Future<Nothing> addFramework;
    EXPECT_CALL(allocator2, addFramework(_, _, _, _))
      .WillOnce(DoAll(InvokeAddFramework(&allocator2),
                      FutureSatisfy(&addFramework)));

    EXPECT_CALL(sched, registered(&driver, _, _));

    Try<Owned<cluster::Master>> master2 = this->StartMaster(&allocator2);
    ASSERT_SOME(master2);

    EXPECT_CALL(sched, disconnected(_));

    // Inform the scheduler about the new master.
    schedulerDetector.appoint(master2.get()->pid);

    AWAIT_READY(addFramework);

    EXPECT_CALL(allocator2, addSlave(_, _, _, _, _, _));

    Future<vector<Offer>> resourceOffers2;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .WillOnce(FutureArg<1>(&resourceOffers2));

    // Inform the slave about the new master.
    slaveDetector.appoint(master2.get()->pid);

    AWAIT_READY(resourceOffers2);

    // Since the task is still running on the slave, the framework
    // should only be offered the resources not being used by the
    // task.
    EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

    EXPECT_CALL(exec, shutdown(_))
      .Times(AtMost(1));

    // Shut everything down.
    driver.stop();
    driver.join();
  }
}


// Checks that in the event of a master failure and the election of a
// new master, if a slave reregisters before a framework that has
// resources on reregisters, all used and unused resources are
// accounted for correctly.
TYPED_TEST(MasterAllocatorTest, SlaveReregistersFirst)
{
  // Objects that persist after the election of a new master.
  StandaloneMasterDetector slaveDetector;
  StandaloneMasterDetector schedulerDetector;
  MockScheduler sched;
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  TestingMesosSchedulerDriver driver(&sched, &schedulerDetector);
  Try<Owned<cluster::Slave>> slave = nullptr;

  // Explicit scope is to ensure all object associated with the
  // leading master (e.g. allocator) are destroyed once the master
  // is shut down. Otherwise subsequent (not expected) calls into
  // Allocator::recoverResources() are possibly rendering tests flaky.
  {
    TestAllocator<TypeParam> allocator;

    EXPECT_CALL(allocator, initialize(_, _, _, _));

    Try<Owned<cluster::Master>> master = this->StartMaster(&allocator);
    ASSERT_SOME(master);

    slaveDetector.appoint(master.get()->pid);
    schedulerDetector.appoint(master.get()->pid);

    EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _));

    slave::Flags flags = this->CreateSlaveFlags();
    flags.resources = Some("cpus:2;mem:1024");

    slave = this->StartSlave(&slaveDetector, &containerizer, flags);
    ASSERT_SOME(slave);

    EXPECT_CALL(allocator, addFramework(_, _, _, _));
    EXPECT_CALL(allocator, recoverResources(_, _, _, _));

    EXPECT_CALL(sched, registered(&driver, _, _));

    // The framework should be offered all of the resources on the
    // slave since it is the only framework running.
    EXPECT_CALL(sched, resourceOffers(&driver, OfferEq(2, 1024)))
      .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 500, "*"))
      .WillRepeatedly(DeclineOffers());

    EXPECT_CALL(exec, registered(_, _, _, _));

    EXPECT_CALL(exec, launchTask(_, _))
      .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

    Future<TaskStatus> status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&status));

    Future<Nothing> _statusUpdateAcknowledgement =
      FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

    driver.start();

    AWAIT_READY(status);

    EXPECT_EQ(TASK_RUNNING, status->state());

    // Make sure the slave handles status update acknowledgement so
    // that it doesn't try to retry the update after master failover.
    AWAIT_READY(_statusUpdateAcknowledgement);

    EXPECT_CALL(allocator, recoverResources(_, _, _, _))
      .WillRepeatedly(DoDefault());
  }

  {
    TestAllocator<TypeParam> allocator2;

    EXPECT_CALL(allocator2, initialize(_, _, _, _));

    Future<Nothing> addSlave;
    EXPECT_CALL(allocator2, addSlave(_, _, _, _, _, _))
      .WillOnce(DoAll(InvokeAddSlave(&allocator2),
                      FutureSatisfy(&addSlave)));

    // Expect the framework to be added but to be inactive when the
    // agent re-registers, until the framework re-registers below.
    Future<Nothing> addFramework;
    EXPECT_CALL(allocator2, addFramework(_, _, _, false))
      .WillOnce(DoAll(InvokeAddFramework(&allocator2),
                      FutureSatisfy(&addFramework)));

    Try<Owned<cluster::Master>> master2 = this->StartMaster(&allocator2);
    ASSERT_SOME(master2);

    // Inform the slave about the new master.
    slaveDetector.appoint(master2.get()->pid);

    AWAIT_READY(addSlave);
    AWAIT_READY(addFramework);

    EXPECT_CALL(sched, disconnected(_));

    EXPECT_CALL(sched, registered(&driver, _, _));

    // Because the framework was re-added above, we expect to only
    // activate the framework when it re-registers.
    EXPECT_CALL(allocator2, addFramework(_, _, _, _))
      .Times(0);

    EXPECT_CALL(allocator2, activateFramework(_));

    Future<vector<Offer>> resourceOffers2;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .WillOnce(FutureArg<1>(&resourceOffers2));

    // Inform the scheduler about the new master.
    schedulerDetector.appoint(master2.get()->pid);

    AWAIT_READY(resourceOffers2);

    // Since the task is still running on the slave, the framework
    // should only be offered the resources not being used by the
    // task.
    EXPECT_THAT(resourceOffers2.get(), OfferEq(1, 524));

    EXPECT_CALL(exec, shutdown(_))
      .Times(AtMost(1));

    // Shut everything down.
    driver.stop();
    driver.join();
  }
}


#ifndef __WINDOWS__
// This test ensures that resource allocation is correctly rebalanced
// according to the updated weights.
TYPED_TEST(MasterAllocatorTest, RebalancedForUpdatedWeights)
{
  Clock::pause();

  TestAllocator<TypeParam> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _, _));

  // Start Mesos master.
  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master =
    this->StartMaster(&allocator, masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  vector<Owned<cluster::Slave>> slaves;

  // Register three agents with the same resources.
  const Resources agentResources = Resources::parse(
      "cpus:2;gpus:0;mem:1024;disk:4096;ports:[31000-32000]").get();

  for (int i = 0; i < 3; i++) {
    Future<Nothing> addSlave;
    EXPECT_CALL(allocator, addSlave(_, _, _, _, _, _))
      .WillOnce(DoAll(InvokeAddSlave(&allocator),
                      FutureSatisfy(&addSlave)));

    slave::Flags flags = this->CreateSlaveFlags();
    flags.resources = stringify(agentResources);

    Try<Owned<cluster::Slave>> slave = this->StartSlave(detector.get(), flags);
    ASSERT_SOME(slave);

    // Advance clock to force agents to register.
    Clock::advance(flags.authentication_backoff_factor);
    Clock::advance(flags.registration_backoff_factor);
    Clock::settle();

    slaves.push_back(slave.get());
    AWAIT_READY(addSlave);
  }

  // Total cluster resources (3 agents): cpus=6, mem=3072.

  // Framework1 registers with 'role1' which uses the default weight (1.0),
  // and all resources will be offered to this framework since it is the only
  // framework running so far.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_role("role1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered1;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureSatisfy(&registered1));

  Future<vector<Offer>> framework1offers1;
  Future<vector<Offer>> framework1offers2;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&framework1offers1))
    .WillOnce(FutureArg<1>(&framework1offers2));

  driver1.start();

  AWAIT_READY(registered1);
  AWAIT_READY(framework1offers1);

  ASSERT_EQ(3u, framework1offers1->size());
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(Resources(framework1offers1.get()[i].resources()),
              allocatedResources(agentResources, "role1"));
  }

  // Framework2 registers with 'role2' which also uses the default weight.
  // It will not get any offers due to all resources having outstanding offers
  // to framework1 when it registered.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_role("role2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  Queue<Offer> framework2offers;
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(EnqueueOffers(&framework2offers));

  driver2.start();
  AWAIT_READY(registered2);

  // Settle to make sure the dispatched allocation is executed before
  // the weights are updated.
  Clock::settle();

  // role1 share = 1 (cpus=6, mem=3072)
  //   framework1 share = 1
  // role2 share = 0
  //   framework2 share = 0

  // Expect offer rescinded.
  EXPECT_CALL(sched1, offerRescinded(&driver1, _)).Times(3);

  Future<Resources> recoverResources1, recoverResources2, recoverResources3;
  EXPECT_CALL(allocator, recoverResources(_, _, _, _))
    .WillOnce(DoAll(FutureArg<2>(&recoverResources1),
                    InvokeRecoverResources(&allocator)))
    .WillOnce(DoAll(FutureArg<2>(&recoverResources2),
                    InvokeRecoverResources(&allocator)))
    .WillOnce(DoAll(FutureArg<2>(&recoverResources3),
                    InvokeRecoverResources(&allocator)))
    .WillRepeatedly(DoDefault());

  // Update the weight of 'role2' to 2.0.
  RepeatedPtrField<WeightInfo> infos = createWeightInfos("role2=2.0");
  Future<Response> response = process::http::request(
      process::http::createRequest(
          master.get()->pid,
          "PUT",
          false,
          "weights",
          createBasicAuthHeaders(DEFAULT_CREDENTIAL),
          strings::format("%s", JSON::protobuf(infos)).get()));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;

  // 'updateWeights' will rescind all outstanding offers and the rescinded
  // offer resources will only be available to the updated weights once
  // another allocation is invoked.
  //
  // TODO(bmahler): This lambda is copied in several places
  // in the code, consider how best to pull this out.
  auto unallocated = [](const Resources& resources) {
    Resources result = resources;
    result.unallocate();
    return result;
  };

  AWAIT_READY(recoverResources1);
  EXPECT_EQ(agentResources, unallocated(recoverResources1.get()));

  AWAIT_READY(recoverResources2);
  EXPECT_EQ(agentResources, unallocated(recoverResources2.get()));

  AWAIT_READY(recoverResources3);
  EXPECT_EQ(agentResources, unallocated(recoverResources3.get()));

  Resources totalRecovered =
    recoverResources1.get() + recoverResources2.get() + recoverResources3.get();
  totalRecovered.unallocate();

  EXPECT_EQ(agentResources + agentResources + agentResources, totalRecovered);

  // Trigger a batch allocation to make sure all resources are
  // offered out again.
  Clock::advance(masterFlags.allocation_interval);

  // Settle to make sure all offers are received.
  Clock::settle();

  // role1 share = 0.33 (cpus=2, mem=1024)
  //   framework1 share = 1
  // role2 share = 0.66 (cpus=4, mem=2048)
  //   framework2 share = 1

  AWAIT_READY(framework1offers2);
  ASSERT_EQ(1u, framework1offers2->size());
  EXPECT_EQ(Resources(framework1offers2.get()[0].resources()),
            allocatedResources(agentResources, "role1"));

  ASSERT_EQ(2u, framework2offers.size());
  for (int i = 0; i < 2; i++) {
    Future<Offer> offer = framework2offers.get();

    // All offers for framework2 are enqueued by now.
    AWAIT_READY(offer);
    EXPECT_EQ(Resources(offer->resources()),
              allocatedResources(agentResources, "role2"));
  }

  driver1.stop();
  driver1.join();
  driver2.stop();
  driver2.join();
}
#endif // __WINDOWS__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
