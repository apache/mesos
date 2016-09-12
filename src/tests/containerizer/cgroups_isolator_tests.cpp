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

#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/format.hpp>
#include <stout/gtest.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

#include "tests/mesos.hpp"
#include "tests/script.hpp"

#include "tests/containerizer/docker_archive.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::CGROUP_SUBSYSTEM_CPU_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_CPUACCT_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_DEVICES_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_MEMORY_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_NET_CLS_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_PERF_EVENT_NAME;
using mesos::internal::slave::CPU_SHARES_PER_CPU_REVOCABLE;
using mesos::internal::slave::DEFAULT_EXECUTOR_CPUS;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;
using process::Queue;

using std::string;
using std::vector;

using testing::InvokeWithoutArgs;

namespace mesos {
namespace internal {
namespace tests {


// Run the balloon framework under a mesos containerizer.
TEST_SCRIPT(ContainerizerTest,
            ROOT_CGROUPS_BalloonFramework,
            "balloon_framework_test.sh")


class CgroupsIsolatorTest : public MesosTest {};


// This test starts the agent with cgroups isolation and launches a
// task with an unprivileged user. Then verifies that the unprivileged
// user has write permission under the corresponding cgroups which are
// prepared for the container to run the task.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_PERF_NET_CLS_UserCgroup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string registry = path::join(os::getcwd(), "registry");

  Future<Nothing> testImage = DockerArchive::create(registry, "alpine");
  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(registry, "alpine.tar")));

  slave::Flags flags = CreateSlaveFlags();
  flags.docker_registry = registry;
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.image_providers = "docker";
  flags.perf_events = "cpu-cycles"; // Needed for `PerfEventSubsystem`.
  flags.isolation =
    "cgroups/cpu,"
    "cgroups/devices,"
    "cgroups/mem,"
    "cgroups/net_cls,"
    "cgroups/perf_event,"
    "docker/runtime,"
    "filesystem/linux";

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get());

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task with the command executor.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/sleep");
  command.add_arguments("sleep");
  command.add_arguments("120");
  command.set_user("nobody");

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  vector<string> subsystems = {
    CGROUP_SUBSYSTEM_CPU_NAME,
    CGROUP_SUBSYSTEM_CPUACCT_NAME,
    CGROUP_SUBSYSTEM_DEVICES_NAME,
    CGROUP_SUBSYSTEM_MEMORY_NAME,
    CGROUP_SUBSYSTEM_NET_CLS_NAME,
    CGROUP_SUBSYSTEM_PERF_EVENT_NAME,
  };

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  foreach (const string& subsystem, subsystems) {
    string hierarchy = path::join(flags.cgroups_hierarchy, subsystem);
    string cgroup = path::join(flags.cgroups_root, containerId.value());

    // Verify that the user cannot manipulate the container's cgroup
    // control files as their owner is root.
    EXPECT_NE(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'echo $$ > %s'",
        path::join(hierarchy, cgroup, "cgroup.procs")).get()));

    // Verify that the user can create a cgroup under the container's
    // cgroup as the isolator changes the owner of the cgroup.
    string userCgroup = path::join(cgroup, "user");

    EXPECT_EQ(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'mkdir %s'",
        path::join(hierarchy, userCgroup)).get()));

    // Verify that the user can manipulate control files in the
    // created cgroup as it's owned by the user.
    EXPECT_EQ(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'echo $$ > %s'",
        path::join(hierarchy, userCgroup, "cgroup.procs")).get()));

    // Clear up the folder.
    AWAIT_READY(cgroups::destroy(hierarchy, userCgroup));
  }

  driver.stop();
  driver.join();
}


TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_RevocableCpu)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu";

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  MockResourceEstimator resourceEstimator;
  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillRepeatedly(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      &resourceEstimator,
      flags);

  ASSERT_SOME(slave);

  // Start the framework which accepts revocable resources.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject an estimation of revocable cpu resources.
  Resource cpu = Resources::parse("cpus", "1", "*").get();
  cpu.mutable_revocable();
  Resources cpus(cpu);
  estimations.put(cpus);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  EXPECT_EQ(cpus, Resources(offers2.get()[0].resources()));

  TaskInfo task = createTask(
      offers2.get()[0].slave_id(),
      cpus,
      "sleep 120");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers2.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  string cpuHierarchy = path::join(flags.cgroups_hierarchy, "cpu");
  string cpuCgroup= path::join(flags.cgroups_root, containerId.value());

  double totalCpus = cpus.cpus().get() + DEFAULT_EXECUTOR_CPUS;
  EXPECT_SOME_EQ(
      CPU_SHARES_PER_CPU_REVOCABLE * totalCpus,
      cgroups::cpu::shares(cpuHierarchy, cpuCgroup));

  driver.stop();
  driver.join();
}


TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CFS_EnableCfs)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu";

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Generate random numbers to max out a single core. We'll run this
  // for 0.5 seconds of wall time so it should consume approximately
  // 250 ms of total cpu time when limited to 0.5 cpu. We use
  // /dev/urandom to prevent blocking on Linux when there's
  // insufficient entropy.
  string command =
    "cat /dev/urandom > /dev/null & "
    "export MESOS_TEST_PID=$! && "
    "sleep 0.5 && "
    "kill $MESOS_TEST_PID";

  ASSERT_GE(Resources(offers.get()[0].resources()).cpus().get(), 0.5);

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:0.5").get(),
      command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // Expect that no more than 300 ms of cpu time has been consumed. We
  // also check that at least 50 ms of cpu time has been consumed so
  // this test will fail if the host system is very heavily loaded.
  // This behavior is correct because under such conditions we aren't
  // actually testing the CFS cpu limiter.
  double cpuTime = usage.get().cpus_system_time_secs() +
                   usage.get().cpus_user_time_secs();

  EXPECT_GE(0.30, cpuTime);
  EXPECT_LE(0.05, cpuTime);

  driver.stop();
  driver.join();
}


// The test verifies that the number of processes and threads in a
// container is correctly reported.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_PidsAndTids)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu";
  flags.cgroups_cpu_enable_pids_and_tids_count = true;

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/cat");
  command.add_arguments("/bin/cat");

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      command);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // The possible running processes during capture process number.
  //   - src/.libs/mesos-executor
  //   - src/mesos-executor
  //   - src/.libs/mesos-containerizer
  //   - src/mesos-containerizer
  //   - cat
  // For `cat` and `mesos-executor`, they keep idling during running
  // the test case. For other processes, they may occur temporarily.
  EXPECT_GE(usage.get().processes(), 2U);
  EXPECT_GE(usage.get().threads(), 2U);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
