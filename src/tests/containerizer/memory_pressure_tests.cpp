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

#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "messages/messages.hpp"

#include "tests/mesos.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using std::vector;

using testing::_;
using testing::Eq;
using testing::Return;
using testing::Unused;

namespace mesos {
namespace internal {
namespace tests {

class MemoryPressureMesosTest : public ContainerizerTest<MesosContainerizer>
{
public:
  static void SetUpTestCase()
  {
    ContainerizerTest<MesosContainerizer>::SetUpTestCase();

    // Verify that the dd command and its flags used in a bit are valid
    // on this system.
    ASSERT_SOME_EQ(0, os::system("dd count=1 bs=1M if=/dev/zero of=/dev/null"))
      << "Cannot find a compatible 'dd' command";
  }
};


TEST_F(MemoryPressureMesosTest, ROOT_CGROUPS_Statistics)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // We only care about memory cgroup for this test.
  flags.isolation = "cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Run a task that triggers memory pressure event. We request 1G
  // disk because we are going to write a 512 MB file repeatedly.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:256;disk:1024").get(),
      "while true; do dd count=512 bs=1M if=/dev/zero of=./temp; done");

  Future<TaskStatus> starting;
  Future<TaskStatus> running;
  Future<TaskStatus> killed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&starting))
    .WillOnce(FutureArg<1>(&running))
    .WillOnce(FutureArg<1>(&killed))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(starting);
  EXPECT_EQ(task.task_id(), starting->task_id());
  EXPECT_EQ(TASK_STARTING, starting->state());

  AWAIT_READY(running);
  EXPECT_EQ(task.task_id(), running->task_id());
  EXPECT_EQ(TASK_RUNNING, running->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  // Wait a while for some memory pressure events to occur.
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    if (usage->mem_low_pressure_counter() > 0) {
      // We will check the correctness of the memory pressure counters
      // later, because the memory-hammering task is still active
      // and potentially incrementing these counters.
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  // Pause the clock to ensure that the reaper doesn't reap the exited
  // command executor and inform the containerizer/slave.
  Clock::pause();
  Clock::settle();

  // Stop the memory-hammering task.
  driver.killTask(task.task_id());

  AWAIT_READY_FOR(killed, Seconds(120));
  EXPECT_EQ(task.task_id(), killed->task_id());
  EXPECT_EQ(TASK_KILLED, killed->state());

  // Now check the correctness of the memory pressure counters.
  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage->mem_low_pressure_counter(),
            usage->mem_medium_pressure_counter());
  EXPECT_GE(usage->mem_medium_pressure_counter(),
            usage->mem_critical_pressure_counter());

  Clock::resume();

  driver.stop();
  driver.join();
}


// Test that memory pressure listening is restarted after recovery.
TEST_F(MemoryPressureMesosTest, ROOT_CGROUPS_SlaveRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // We only care about memory cgroup for this test.
  flags.isolation = "cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Run a task that triggers memory pressure event. We request 1G
  // disk because we are going to write a 512 MB file repeatedly.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:256;disk:1024").get(),
      "while true; do dd count=512 bs=1M if=/dev/zero of=./temp; done");

  Future<TaskStatus> starting;
  Future<TaskStatus> running;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&starting))
    .WillOnce(FutureArg<1>(&running))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  Future<Nothing> runningAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> startingAck =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(starting);
  EXPECT_EQ(task.task_id(), starting->task_id());
  EXPECT_EQ(TASK_STARTING, starting->state());

  AWAIT_READY_FOR(startingAck, Seconds(120));

  AWAIT_READY(running);
  EXPECT_EQ(task.task_id(), running->task_id());
  EXPECT_EQ(TASK_RUNNING, running->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY_FOR(runningAck, Seconds(120));

  // We restart the slave to let it recover.
  slave.get()->terminate();

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has reregistered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  // Use the same flags.
  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<SlaveReregisteredMessage> reregistered =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregistered);

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  // Wait a while for some memory pressure events to occur.
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    if (usage->mem_low_pressure_counter() > 0) {
      // We will check the correctness of the memory pressure counters
      // later, because the memory-hammering task is still active
      // and potentially incrementing these counters.
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  // Pause the clock to ensure that the reaper doesn't reap the exited
  // command executor and inform the containerizer/slave.
  Clock::pause();
  Clock::settle();

  Future<TaskStatus> killed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killed));

  // Stop the memory-hammering task.
  driver.killTask(task.task_id());

  AWAIT_READY_FOR(killed, Seconds(120));
  EXPECT_EQ(task.task_id(), killed->task_id());
  EXPECT_EQ(TASK_KILLED, killed->state());

  // Now check the correctness of the memory pressure counters.
  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage->mem_low_pressure_counter(),
            usage->mem_medium_pressure_counter());
  EXPECT_GE(usage->mem_medium_pressure_counter(),
            usage->mem_critical_pressure_counter());

  Clock::resume();

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
