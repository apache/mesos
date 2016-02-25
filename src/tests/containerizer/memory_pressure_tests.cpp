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
    // Verify that the dd command and its flags used in a bit are valid
    // on this system.
    ASSERT_EQ(0, os::system("dd count=1 bs=1M if=/dev/zero of=/dev/null"))
      << "Cannot find a compatible 'dd' command";
  }
};


TEST_F(MemoryPressureMesosTest, CGROUPS_ROOT_Statistics)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // We only care about memory cgroup for this test.
  flags.isolation = "cgroups/mem";
  flags.slave_subsystems = None();

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer);

  Future<SlaveRegisteredMessage> registered =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  Try<PID<Slave>> slave = StartSlave(containerizer.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registered);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];

  // Run a task that triggers memory pressure event. We request 1G
  // disk because we are going to write a 512 MB file repeatedly.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:256;disk:1024").get(),
      "while true; do dd count=512 bs=1M if=/dev/zero of=./temp; done");

  Future<TaskStatus> running;
  Future<TaskStatus> killed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&running))
    .WillOnce(FutureArg<1>(&killed))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(running);
  EXPECT_EQ(task.task_id(), running.get().task_id());
  EXPECT_EQ(TASK_RUNNING, running.get().state());

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  // Wait a while for some memory pressure events to occur.
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    if (usage.get().mem_low_pressure_counter() > 0) {
      // We will check the correctness of the memory pressure counters
      // later, because the memory-hammering task is still active
      // and potentially incrementing these counters.
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  // Stop the memory-hammering task.
  driver.killTask(task.task_id());

  AWAIT_READY(killed);
  EXPECT_EQ(task.task_id(), killed->task_id());
  EXPECT_EQ(TASK_KILLED, killed->state());

  // Now check the correctness of the memory pressure counters.
  Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage.get().mem_low_pressure_counter(),
            usage.get().mem_medium_pressure_counter());
  EXPECT_GE(usage.get().mem_medium_pressure_counter(),
            usage.get().mem_critical_pressure_counter());

  driver.stop();
  driver.join();

  Shutdown();
  delete containerizer.get();
}


// Test that memory pressure listening is restarted after recovery.
TEST_F(MemoryPressureMesosTest, CGROUPS_ROOT_SlaveRecovery)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // We only care about memory cgroup for this test.
  flags.isolation = "cgroups/mem";
  flags.slave_subsystems = None();

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer1);

  Future<SlaveRegisteredMessage> registered =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  Try<PID<Slave>> slave = StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(registered);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];

  // Run a task that triggers memory pressure event. We request 1G
  // disk because we are going to write a 512 MB file repeatedly.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:256;disk:1024").get(),
      "while true; do dd count=512 bs=1M if=/dev/zero of=./temp; done");

  Future<TaskStatus> running;
  Promise<TaskStatus> killed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&running))
    .WillRepeatedly(DoAll(
        Invoke([&killed](Unused, const TaskStatus& status) {
          // More than one TASK_RUNNING status can arrive
          // before the TASK_KILLED does.
          if (status.state() == TASK_KILLED) {
            killed.set(status);
          }
        }),
        Return()));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(running);
  EXPECT_EQ(task.task_id(), running.get().task_id());
  EXPECT_EQ(TASK_RUNNING, running.get().state());

  // We restart the slave to let it recover.
  Stop(slave.get());
  delete containerizer1.get();

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has re-registered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  // Use the same flags.
  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(containerizer2);

  Future<SlaveReregisteredMessage> reregistered =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregistered);

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  Future<hashset<ContainerID>> containers = containerizer2.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  // Wait a while for some memory pressure events to occur.
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage = containerizer2.get()->usage(containerId);
    AWAIT_READY(usage);

    if (usage.get().mem_low_pressure_counter() > 0) {
      // We will check the correctness of the memory pressure counters
      // later, because the memory-hammering task is still active
      // and potentially incrementing these counters.
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  // Stop the memory-hammering task.
  driver.killTask(task.task_id());

  AWAIT_READY(killed.future());
  EXPECT_EQ(task.task_id(), killed.future()->task_id());
  EXPECT_EQ(TASK_KILLED, killed.future()->state());

  // Now check the correctness of the memory pressure counters.
  Future<ResourceStatistics> usage = containerizer2.get()->usage(containerId);
  AWAIT_READY(usage);

  EXPECT_GE(usage.get().mem_low_pressure_counter(),
            usage.get().mem_medium_pressure_counter());
  EXPECT_GE(usage.get().mem_medium_pressure_counter(),
            usage.get().mem_critical_pressure_counter());

  driver.stop();
  driver.join();

  Shutdown();
  delete containerizer2.get();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
