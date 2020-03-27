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

#include <limits>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/format.hpp>
#include <stout/gtest.hpp>

#include <stout/os/exec.hpp>

#include <mesos/v1/scheduler.hpp>

#include "slave/gc_process.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.hpp"

#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"
#include "tests/script.hpp"

#include "tests/containerizer/docker_archive.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::CGROUP_SUBSYSTEM_BLKIO_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_CPU_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_CPUACCT_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_CPUSET_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_DEVICES_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_HUGETLB_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_MEMORY_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_NET_CLS_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_NET_PRIO_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_PERF_EVENT_NAME;
using mesos::internal::slave::CGROUP_SUBSYSTEM_PIDS_NAME;
using mesos::internal::slave::CPU_SHARES_PER_CPU;
using mesos::internal::slave::CPU_SHARES_PER_CPU_REVOCABLE;
using mesos::internal::slave::CPU_CFS_PERIOD;
using mesos::internal::slave::DEFAULT_EXECUTOR_CPUS;
using mesos::internal::slave::DEFAULT_EXECUTOR_MEM;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::NetClsHandle;
using mesos::internal::slave::NetClsHandleManager;
using mesos::internal::slave::Slave;

using mesos::internal::slave::containerizer::paths::getCgroupPath;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::Owned;
using process::Queue;

using process::http::OK;
using process::http::Response;

using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::AllOf;
using testing::DoAll;
using testing::InvokeWithoutArgs;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


// Run the balloon framework under a mesos containerizer.
TEST_SCRIPT(ContainerizerTest,
            ROOT_CGROUPS_BalloonFramework,
            "balloon_framework_test.sh")


class CgroupsIsolatorTest
  : public ContainerizerTest<MesosContainerizer> {};


// This test starts the agent with cgroups isolation and launches a
// task with an unprivileged user. Then verifies that the unprivileged
// user has write permission under the corresponding cgroups which are
// prepared for the container to run the task.
TEST_F(CgroupsIsolatorTest,
       ROOT_CGROUPS_PERF_NET_CLS_UNPRIVILEGED_USER_UserCgroup)
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

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  // Launch a task with the command executor.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/sleep");
  command.add_arguments("sleep");
  command.add_arguments("120");
  command.set_user(user.get());

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

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
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  foreach (const string& subsystem, subsystems) {
    Result<string> hierarchy = cgroups::hierarchy(subsystem);
    ASSERT_SOME(hierarchy);

    string cgroup = path::join(flags.cgroups_root, containerId.value());

    // Verify that the user cannot manipulate the container's cgroup
    // control files as their owner is root.
    EXPECT_SOME_NE(0, os::system(strings::format(
        "su - %s -s /bin/sh -c 'echo $$ > %s'",
        user.get(),
        path::join(hierarchy.get(), cgroup, "cgroup.procs")).get()));

    // Verify that the user can create a cgroup under the container's
    // cgroup as the isolator changes the owner of the cgroup.
    string userCgroup = path::join(cgroup, "user");

    EXPECT_SOME_EQ(0, os::system(strings::format(
        "su - %s -s /bin/sh -c 'mkdir %s'",
        user.get(),
        path::join(hierarchy.get(), userCgroup)).get()));

    // Verify that the user can manipulate control files in the
    // created cgroup as it's owned by the user.
    EXPECT_SOME_EQ(0, os::system(strings::format(
        "su - %s -s /bin/sh -c 'echo $$ > %s'",
        user.get(),
        path::join(hierarchy.get(), userCgroup, "cgroup.procs")).get()));

    // Clear up the folder.
    AWAIT_READY(cgroups::destroy(hierarchy.get(), userCgroup));
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

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
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
  ASSERT_FALSE(offers2->empty());
  EXPECT_EQ(allocatedResources(cpus, frameworkInfo.roles(0)),
            Resources(offers2.get()[0].resources()));

  TaskInfo task = createTask(
      offers2.get()[0].slave_id(),
      cpus,
      "sleep 120");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers2.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  string cpuCgroup = path::join(flags.cgroups_root, containerId.value());

  double totalCpus = cpus.cpus().get() + DEFAULT_EXECUTOR_CPUS;
  EXPECT_SOME_EQ(
      CPU_SHARES_PER_CPU_REVOCABLE * totalCpus,
      cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup));

  driver.stop();
  driver.join();
}


// This test verifies that a task launched with 0.5 cpu and 32MB memory as its
// resource requests (but no resource limits specified) will have its CPU and
// memory's soft & hard limits and OOM score adjustment set correctly.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CFS_CommandTaskNoLimits)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";

  // Enable CFS to cap CPU utilization.
  flags.cgroups_enable_cfs = true;

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // We will launch a task with 0.5 cpu and 32MB memory, and the command
  // executor will be given 0.1 cpu (`DEFAULT_EXECUTOR_CPUS`) and 32MB
  // memory (DEFAULT_EXECUTOR_MEM) by default, so we need 0.6 cpu and 64MB
  // in total.
  ASSERT_GE(
      Resources(offers.get()[0].resources()).cpus().get(),
      0.5 + DEFAULT_EXECUTOR_CPUS);

  ASSERT_GE(
       Resources(offers.get()[0].resources()).mem().get(),
       Megabytes(32) + DEFAULT_EXECUTOR_MEM);

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:0.5;mem:32").get(),
      SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Result<string> memoryHierarchy = cgroups::hierarchy("memory");
  ASSERT_SOME(memoryHierarchy);

  string cgroup = path::join(flags.cgroups_root, containerId.value());

  // Ensure the CPU shares and CFS quota are correctly set for the container.
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * (0.5 + DEFAULT_EXECUTOR_CPUS)),
      cgroups::cpu::shares(cpuHierarchy.get(), cgroup));

  Try<Duration> cfsQuota =
    cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), cgroup);

  ASSERT_SOME(cfsQuota);

  double expectedCFSQuota = (0.5 + DEFAULT_EXECUTOR_CPUS) * CPU_CFS_PERIOD.ms();
  EXPECT_EQ(expectedCFSQuota, cfsQuota->ms());

  // Ensure the memory soft and hard limits are correctly set for the container.
  EXPECT_SOME_EQ(
      Megabytes(32) + DEFAULT_EXECUTOR_MEM,
      cgroups::memory::soft_limit_in_bytes(memoryHierarchy.get(), cgroup));

  EXPECT_SOME_EQ(
      Megabytes(32) + DEFAULT_EXECUTOR_MEM,
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), cgroup));

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Ensure the OOM score adjustment is set to the default value (i.e. 0).
  Try<string> read = os::read(
      strings::format("/proc/%d/oom_score_adj", status->executor_pid()).get());

  ASSERT_SOME(read);

  Try<int32_t> oomScoreAdj = numify<int32_t>(strings::trim(read.get()));
  ASSERT_SOME_EQ(0, oomScoreAdj);

  driver.stop();
  driver.join();
}


// This test verifies that a task launched with resource limits specified
// will have its CPU and memory's soft & hard limits and OOM score adjustment
// set correctly.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CFS_CommandTaskLimits)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start agent with 2 CPUs, total host memory and 1024MB disk.
  Try<os::Memory> memory = os::memory();
  ASSERT_SOME(memory);

  uint64_t totalMemInMB = memory->total.bytes() / 1024 / 1024;

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";
  flags.resources =
    strings::format("cpus:2;mem:%d;disk:1024", totalMemInMB).get();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task with 0.2 cpu request, 0.5 cpu limit, half of
  // host total memory - `DEFAULT_EXECUTOR_MEM` as memory request
  // and half of host total memory as memory limit.
  string resourceRequests = strings::format(
      "cpus:0.2;mem:%d;disk:1024",
      totalMemInMB/2 - DEFAULT_EXECUTOR_MEM.bytes() / 1024 / 1024).get();

  Value::Scalar cpuLimit, memLimit;
  cpuLimit.set_value(0.5);
  memLimit.set_value(totalMemInMB/2);

  google::protobuf::Map<string, Value::Scalar> resourceLimits;
  resourceLimits.insert({"cpus", cpuLimit});
  resourceLimits.insert({"mem", memLimit});

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse(resourceRequests).get(),
      SLEEP_COMMAND(1000),
      None(),
      "test-task",
      id::UUID::random().toString(),
      resourceLimits);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Result<string> memoryHierarchy = cgroups::hierarchy("memory");
  ASSERT_SOME(memoryHierarchy);

  string cgroup = path::join(flags.cgroups_root, containerId.value());

  // The command executor will be given 0.1 cpu (`DEFAULT_EXECUTOR_CPUS`)
  // by default, so in total the CPU shares of the executor container
  // should be 0.3.
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.3),
      cgroups::cpu::shares(cpuHierarchy.get(), cgroup));

  // The 0.1 cpu given to the command executor is also included in the cpu
  // limit, so in total the CFS quota should be 0.6.
  Try<Duration> cfsQuota =
    cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), cgroup);

  ASSERT_SOME(cfsQuota);

  double expectedCFSQuota =
    (cpuLimit.value() + DEFAULT_EXECUTOR_CPUS) * CPU_CFS_PERIOD.ms();

  EXPECT_EQ(expectedCFSQuota, cfsQuota->ms());

  // The command executor will be given 32MB (`DEFAULT_EXECUTOR_MEM`) by default
  // so in total the memory soft limit should be half of host total memory.
  EXPECT_SOME_EQ(
      Megabytes(totalMemInMB/2),
      cgroups::memory::soft_limit_in_bytes(memoryHierarchy.get(), cgroup));

  // The 32MB memory given to the command executor is also included in the
  // memory limit, so in total the memory limit should be half of host total
  // memory + 32MB.
  EXPECT_SOME_EQ(
      Megabytes(memLimit.value()) + DEFAULT_EXECUTOR_MEM,
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), cgroup));

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Ensure the OOM score adjustment is correctly set for the container.
  Try<string> read = os::read(
      strings::format("/proc/%d/oom_score_adj", status->executor_pid()).get());

  ASSERT_SOME(read);

  // Since the memory request is half of host total memory (please note that
  // `DEFAULT_EXECUTOR_MEM` is also included in memory request), so the OOM
  // score adjustment should be about 500, see `MemorySubsystemProcess::isolate`
  // for the detailed algorithm.
  Try<int32_t> oomScoreAdj = numify<int32_t>(strings::trim(read.get()));
  ASSERT_SOME(oomScoreAdj);

  EXPECT_GT(502, oomScoreAdj.get());
  EXPECT_LT(498, oomScoreAdj.get());

  driver.stop();
  driver.join();
}


// This test verifies that a task launched with infinite resource
// limits specified will have its CPU and memory's hard limits set
// correctly to infinite values.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CFS_CommandTaskInfiniteLimits)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task with infinite resource limits.
  Value::Scalar cpuLimit, memLimit;
  cpuLimit.set_value(std::numeric_limits<double>::infinity());
  memLimit.set_value(std::numeric_limits<double>::infinity());

  google::protobuf::Map<string, Value::Scalar> resourceLimits;
  resourceLimits.insert({"cpus", cpuLimit});
  resourceLimits.insert({"mem", memLimit});

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      "sleep 1000",
      None(),
      "test-task",
      id::UUID::random().toString(),
      resourceLimits);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Result<string> memoryHierarchy = cgroups::hierarchy("memory");
  ASSERT_SOME(memoryHierarchy);

  string cgroup = path::join(flags.cgroups_root, containerId.value());

  // The CFS quota should be -1 which means infinite quota.
  Try<string> quota =
    cgroups::read(cpuHierarchy.get(), cgroup, "cpu.cfs_quota_us");

  ASSERT_SOME(quota);
  EXPECT_EQ("-1", strings::trim(quota.get()));

  // Root cgroup (e.g., `/sys/fs/cgroup/memory/`) cannot have any limits set, so
  // its hard limit must be infinity.
  Try<Bytes> rootCgrouplimit =
    cgroups::memory::limit_in_bytes(memoryHierarchy.get(), "");

  ASSERT_SOME(rootCgrouplimit);

  // The memory hard limit should be same as root cgroup's, i.e. infinity.
  EXPECT_SOME_EQ(
      rootCgrouplimit.get(),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), cgroup));

  driver.stop();
  driver.join();
}


// This test verifies the default executor container's CPU and memory
// soft & hard limits can be updated correctly when launching task groups
// and killing tasks, and also verifies task's CPU and memory soft & hard
// limits can be set correctly.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CFS_TaskGroupLimits)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";
  flags.cgroups_enable_cfs = true;
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer1 = offers1->offers(0);
  const v1::AgentID& agentId = offer1.agent_id();

  // Launch the first task group which has two tasks, task1 has no resource
  // limits specified but task2 has.
  v1::TaskInfo taskInfo1 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      SLEEP_COMMAND(1000));

  mesos::v1::Value::Scalar cpuLimit, memLimit;
  cpuLimit.set_value(0.5);
  memLimit.set_value(64);

  google::protobuf::Map<string, mesos::v1::Value::Scalar> resourceLimits;
  resourceLimits.insert({"cpus", cpuLimit});
  resourceLimits.insert({"mem", memLimit});

  v1::TaskInfo taskInfo2 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      SLEEP_COMMAND(1000),
      None(),
      "test-task",
      id::UUID::random().toString(),
      resourceLimits);

  taskInfo1.mutable_container()->set_type(mesos::v1::ContainerInfo::MESOS);
  taskInfo1.mutable_container()->mutable_linux_info()->set_share_cgroups(false);
  taskInfo2.mutable_container()->set_type(mesos::v1::ContainerInfo::MESOS);
  taskInfo2.mutable_container()->mutable_linux_info()->set_share_cgroups(false);

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task1)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate1),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> killedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_KILLED))))
    .InSequence(task2)
    .WillOnce(
        DoAll(
            FutureArg<1>(&killedUpdate2),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());

  {
    v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
        executorInfo,
        v1::createTaskGroupInfo({taskInfo1, taskInfo2}));

    Call call = v1::createCallAccept(frameworkId, offer1, {launchGroup});

    // Set a 0s filter to immediately get another offer to launch
    // the second task group.
    call.mutable_accept()->mutable_filters()->set_refuse_seconds(0);

    mesos.send(call);
  }

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(runningUpdate1);

  AWAIT_READY(startingUpdate2);
  AWAIT_READY(runningUpdate2);

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(3u, containers->size());

  // Get task container IDs.
  const v1::ContainerStatus& containerStatus1 =
    runningUpdate1->status().container_status();

  ASSERT_TRUE(containerStatus1.has_container_id());
  ASSERT_TRUE(containerStatus1.container_id().has_parent());

  const v1::ContainerID& taskContainerId1 = containerStatus1.container_id();

  const v1::ContainerStatus& containerStatus2 =
    runningUpdate2->status().container_status();

  ASSERT_TRUE(containerStatus2.has_container_id());
  ASSERT_TRUE(containerStatus2.container_id().has_parent());

  const v1::ContainerID& taskContainerId2 = containerStatus2.container_id();

  EXPECT_EQ(taskContainerId1.parent(), taskContainerId2.parent());

  // Get the executor container ID.
  const v1::ContainerID& executorContainerId = taskContainerId1.parent();

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Result<string> memoryHierarchy = cgroups::hierarchy("memory");
  ASSERT_SOME(memoryHierarchy);

  const string& executorCgroup =
    path::join(flags.cgroups_root, executorContainerId.value());

  const string& taskCgroup1 =
    getCgroupPath(flags.cgroups_root, devolve(taskContainerId1));

  const string& taskCgroup2 =
    getCgroupPath(flags.cgroups_root, devolve(taskContainerId2));

  // The CPU shares of the executor container is the sum of its own CPU
  // request (0.1) + task1's CPU request (0.1) + task2's CPU request (0.1),
  // i.e. 0.3.
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.3),
      cgroups::cpu::shares(cpuHierarchy.get(), executorCgroup));

  // The CPU shares of task1 is its CPU request (0.1).
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.1),
      cgroups::cpu::shares(cpuHierarchy.get(), taskCgroup1));

  // The CPU shares of task2 is its CPU request (0.1).
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.1),
      cgroups::cpu::shares(cpuHierarchy.get(), taskCgroup2));

  // The CFS quota of the executor container is the sum of its own CPU
  // request (0.1) + task1's CPU request (0.1) + task2's CPU limit (0.5),
  // i.e. 0.7.
  Try<Duration> cfsQuota =
    cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), executorCgroup);

  ASSERT_SOME(cfsQuota);
  EXPECT_EQ(0.7 * CPU_CFS_PERIOD.ms(), cfsQuota->ms());

  // The CFS quota of task1 is its CPU request (0.1).
  cfsQuota = cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), taskCgroup1);
  ASSERT_SOME(cfsQuota);
  EXPECT_EQ(0.1 * CPU_CFS_PERIOD.ms(), cfsQuota->ms());

  // The CFS quota of task2 is its CPU limit (0.5).
  cfsQuota = cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), taskCgroup2);
  ASSERT_SOME(cfsQuota);
  EXPECT_EQ(0.5 * CPU_CFS_PERIOD.ms(), cfsQuota->ms());

  // The memory soft limit of the executor container is the sum of its
  // own memory request (32MB) + task1's memory request (32MB) + task2's
  // memory request (32MB), i.e. 96MB.
  EXPECT_SOME_EQ(
      Megabytes(96),
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), executorCgroup));

  // The memory soft limit of task1 is its memory request (32MB).
  EXPECT_SOME_EQ(
      Megabytes(32),
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), taskCgroup1));

  // The memory soft limit of task2 is its memory request (32MB).
  EXPECT_SOME_EQ(
      Megabytes(32),
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), taskCgroup2));

  // The memory hard limit of the executor container is the sum of its
  // own memory request (32MB) + task1's memory request (32MB) + task2's
  // memory limit (64MB), i.e. 128MB.
  EXPECT_SOME_EQ(
      Megabytes(128),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), executorCgroup));

  // The memory hard limit of task1 is its memory request (32MB).
  EXPECT_SOME_EQ(
      Megabytes(32),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), taskCgroup1));

  // The memory hard limit of task2 is its memory limit (64MB).
  EXPECT_SOME_EQ(
      Megabytes(64),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), taskCgroup2));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  // Launch the second task group which has only one task: task3, and this
  // task has no resource limits specified.
  v1::TaskInfo taskInfo3 = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      SLEEP_COMMAND(1000));

  taskInfo3.mutable_container()->set_type(mesos::v1::ContainerInfo::MESOS);
  taskInfo3.mutable_container()->mutable_linux_info()->set_share_cgroups(false);

  Future<v1::scheduler::Event::Update> startingUpdate3;
  Future<v1::scheduler::Event::Update> runningUpdate3;

  testing::Sequence task3;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&startingUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo3.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task3)
    .WillOnce(
        DoAll(
            FutureArg<1>(&runningUpdate3),
            v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer2,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo3}))}));

  AWAIT_READY(startingUpdate3);
  AWAIT_READY(runningUpdate3);

  // The CPU shares of the executor container is the sum of its own CPU
  // request (0.1) + task1's CPU request (0.1) + task2's CPU request (0.1)
  // + task3's CPU request (0.1), i.e. 0.4.
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.4),
      cgroups::cpu::shares(cpuHierarchy.get(), executorCgroup));

  // The CFS quota of the executor container is the sum of its own CPU
  // request (0.1) + task1's CPU request (0.1) + task2's CPU limit (0.5)
  // + task3's CPU request (0.1), i.e. 0.8.
  cfsQuota = cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), executorCgroup);
  ASSERT_SOME(cfsQuota);
  EXPECT_EQ(0.8 * CPU_CFS_PERIOD.ms(), cfsQuota->ms());

  // The memory soft limit of the executor container is the sum of its
  // own memory request (32MB) + task1's memory request (32MB) + task2's
  // memory request (32MB) + task3's memory request (32MB) i.e. 128MB.
  EXPECT_SOME_EQ(
      Megabytes(128),
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), executorCgroup));

  // The memory hard limit of the executor container is the sum of its
  // own memory request (32MB) + task1's memory request (32MB) + task2's
  // memory limit (64MB) + task3's memory request (32MB), i.e. 160MB.
  EXPECT_SOME_EQ(
      Megabytes(160),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), executorCgroup));

  // Now kill a task in the first task group.
  mesos.send(v1::createCallKill(frameworkId, taskInfo1.task_id()));

  // Both of the two tasks in the first group will be killed.
  AWAIT_READY(killedUpdate1);
  AWAIT_READY(killedUpdate2);

  // The CPU shares of the executor container is the sum of its own CPU
  // request (0.1) + task3's CPU request (0.1), i.e. 0.2.
  EXPECT_SOME_EQ(
      (uint64_t)(CPU_SHARES_PER_CPU * 0.2),
      cgroups::cpu::shares(cpuHierarchy.get(), executorCgroup));

  // The CFS quota of the executor container is also the sum of its own CPU
  // request (0.1) + task3's CPU request (0.1), i.e. 0.2.
  cfsQuota = cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), executorCgroup);
  ASSERT_SOME(cfsQuota);
  EXPECT_EQ(0.2 * CPU_CFS_PERIOD.ms(), cfsQuota->ms());

  // The memory soft limit of the executor container is the sum of its
  // own memory request (32MB) + task3's memory request (32MB) i.e. 64MB.
  EXPECT_SOME_EQ(
      Megabytes(64),
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), executorCgroup));

  // We only update the memory hard limit if it is the first time or when
  // we're raising the existing limit (see `MemorySubsystemProcess::update`
  // for details). So now the memory hard limit of the executor container
  // should still be 160MB.
  EXPECT_SOME_EQ(
      Megabytes(160),
      cgroups::memory::limit_in_bytes(memoryHierarchy.get(), executorCgroup));
}


// This test verifies the limit swap functionality. Note that We use
// the default executor here in order to exercise both the increasing
// and decreasing of the memory limit.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_LimitSwap)
{
  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,cgroups/mem";
  flags.cgroups_limit_swap = true;
  flags.authenticate_http_readwrite = false;

  // TODO(jieyu): Add a test filter for memsw support.
  Result<Bytes> check = cgroups::memory::memsw_limit_in_bytes(
      path::join(flags.cgroups_hierarchy, "memory"), "/");

  ASSERT_FALSE(check.isError());

  if (check.isNone()) {
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo("ls", {"ls", "-al", "/"}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateRunning->status().task_id());
  EXPECT_TRUE(updateRunning->status().has_timestamp());

  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateFinished));

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
  EXPECT_TRUE(updateFinished->status().has_timestamp());
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

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/cat");
  command.add_arguments("/bin/cat");

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

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
  EXPECT_GE(usage->processes(), 2U);
  EXPECT_GE(usage->threads(), 2U);

  driver.stop();
  driver.join();
}


// This tests the creation of cgroup when cgoups_root dir is gone.
// All tasks will fail if this happens after slave starting/recovering.
// We should create cgroup recursively to solve this. SEE MESOS-9305.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CreateRecursively)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);
  ASSERT_SOME(slave);

  // Wait until agent recovery is complete.
  AWAIT_READY(__recover);

  Result<string> hierarchy = cgroups::hierarchy("memory");
  ASSERT_SOME(hierarchy);

  // We should remove cgroups_root after the slave being started
  // because slave will create cgroups_root dir during startup
  // if it's not present.
  ASSERT_SOME(os::rmdir(
        path::join(hierarchy.get(), flags.cgroups_root), false));
  ASSERT_FALSE(os::exists(flags.cgroups_root));

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Create a task to be launched in the mesos-container. We will be
  // explicitly killing this task to perform the cleanup test.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the update to verify that the task has been launched.
  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  // Task is ready. Make sure there is exactly 1 container in the hashset.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerID = *(containers->begin());

  // Check if the memory cgroup for this container exists, by
  // checking for the processes associated with this cgroup.
  string cgroup = path::join(
      flags.cgroups_root,
      containerID.value());

  Try<set<pid_t>> pids = cgroups::processes(hierarchy.get(), cgroup);
  ASSERT_SOME(pids);

  // There should be at least one TGID associated with this cgroup.
  EXPECT_LE(1u, pids->size());

  // Isolator cleanup test: Killing the task should cleanup the cgroup
  // associated with the container.
  Future<TaskStatus> killStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&killStatus));

  // Wait for the executor to exit. We are using 'gc.schedule' as a proxy event
  // to monitor the exit of the executor.
  Future<Nothing> gcSchedule = FUTURE_DISPATCH(
      _, &slave::GarbageCollectorProcess::schedule);

  driver.killTask(statusRunning->task_id());

  AWAIT_READY(gcSchedule);

  // If the cleanup is successful the memory cgroup for this container should
  // not exist.
  ASSERT_FALSE(os::exists(cgroup));

  driver.stop();
  driver.join();
}


class NetClsHandleManagerTest : public testing::Test {};


// Tests the ability of the `NetClsHandleManager` class to allocate
// and free secondary handles from a range of primary handles.
TEST_F(NetClsHandleManagerTest, AllocateFreeHandles)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  Try<NetClsHandle> handle = manager.alloc(0x0003);
  ASSERT_SOME(handle);

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  ASSERT_SOME(manager.free(handle.get()));

  EXPECT_SOME_FALSE(manager.isUsed(handle.get()));
}


// Make sure allocation of secondary handles for invalid primary
// handles results in an error.
TEST_F(NetClsHandleManagerTest, AllocateInvalidPrimary)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  ASSERT_ERROR(manager.alloc(0x0001));
}


// Tests that we can reserve secondary handles for a given primary
// handle so that they won't be allocated out later.
TEST_F(NetClsHandleManagerTest, ReserveHandles)
{
  NetClsHandleManager manager(IntervalSet<uint32_t>(
      (Bound<uint32_t>::closed(0x0002),
       Bound<uint32_t>::closed(0x0003))));

  NetClsHandle handle(0x0003, 0xffff);

  ASSERT_SOME(manager.reserve(handle));

  EXPECT_SOME_TRUE(manager.isUsed(handle));
}


// Tests that secondary handles are allocated only from a given range,
// when the range is specified.
TEST_F(NetClsHandleManagerTest, SecondaryHandleRange)
{
  NetClsHandleManager manager(
      IntervalSet<uint32_t>(
        (Bound<uint32_t>::closed(0x0002),
         Bound<uint32_t>::closed(0x0003))),
      IntervalSet<uint32_t>(
        (Bound<uint32_t>::closed(0xffff),
         Bound<uint32_t>::closed(0xffff))));

  Try<NetClsHandle> handle = manager.alloc(0x0003);
  ASSERT_SOME(handle);

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  // Try allocating another handle. This should fail, since we don't
  // have any more secondary handles left.
  EXPECT_ERROR(manager.alloc(0x0003));

  ASSERT_SOME(manager.free(handle.get()));

  ASSERT_SOME(manager.reserve(handle.get()));

  EXPECT_SOME_TRUE(manager.isUsed(handle.get()));

  // Make sure you cannot reserve a secondary handle that is out of
  // range.
  EXPECT_ERROR(manager.reserve(NetClsHandle(0x0003, 0x0001)));
}


// This tests the create, prepare, isolate and cleanup methods of the
// 'CgroupNetClsIsolatorProcess'. The test first creates a
// 'MesosContainerizer' with net_cls cgroup isolator enabled. The
// net_cls cgroup isolator is implemented in the
// 'CgroupNetClsIsolatorProcess' class. The test then launches a task
// in a mesos container and checks to see if the container has been
// added to the right net_cls cgroup. Finally, the test kills the task
// and makes sure that the 'CgroupNetClsIsolatorProcess' cleans up the
// net_cls cgroup created for the container.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_NET_CLS_Isolate)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  uint16_t primary = 0x0012;

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/net_cls";
  flags.cgroups_net_cls_primary_handle = stringify(primary);
  flags.cgroups_net_cls_secondary_handles = "0xffff,0xffff";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Create a task to be launched in the mesos-container. We will be
  // explicitly killing this task to perform the cleanup test.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the update to verify that the task has been launched.
  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  // Task is ready. Make sure there is exactly 1 container in the hashset.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerID = *(containers->begin());

  Result<string> hierarchy = cgroups::hierarchy("net_cls");
  ASSERT_SOME(hierarchy);

  // Check if the net_cls cgroup for this container exists, by
  // checking for the processes associated with this cgroup.
  string cgroup = path::join(
      flags.cgroups_root,
      containerID.value());

  Try<set<pid_t>> pids = cgroups::processes(hierarchy.get(), cgroup);
  ASSERT_SOME(pids);

  // There should be at least one TGID associated with this cgroup.
  EXPECT_LE(1u, pids->size());

  // Read the `net_cls.classid` to verify that the handle has been set.
  Try<uint32_t> classid = cgroups::net_cls::classid(hierarchy.get(), cgroup);
  EXPECT_SOME(classid);

  if (classid.isSome()) {
    // Make sure the primary handle is the same as the one set in
    // `--cgroup_net_cls_primary_handle`.
    EXPECT_EQ(primary, (classid.get() & 0xffff0000) >> 16);

    // Make sure the secondary handle is 0xffff.
    EXPECT_EQ(0xffffu, classid.get() & 0xffff);
  }

  // Isolator cleanup test: Killing the task should cleanup the cgroup
  // associated with the container.
  Future<TaskStatus> killStatus;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&killStatus));

  // Wait for the executor to exit. We are using 'gc.schedule' as a proxy event
  // to monitor the exit of the executor.
  Future<Nothing> gcSchedule = FUTURE_DISPATCH(
      _, &slave::GarbageCollectorProcess::schedule);

  driver.killTask(statusRunning->task_id());

  AWAIT_READY(gcSchedule);

  // If the cleanup is successful the net_cls cgroup for this container should
  // not exist.
  ASSERT_FALSE(os::exists(cgroup));

  driver.stop();
  driver.join();
}


// This test verifies that we are able to retrieve the `net_cls` handle
// from `/state`.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_NET_CLS_ContainerStatus)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/net_cls";
  flags.cgroups_net_cls_primary_handle = stringify(0x0012);
  flags.cgroups_net_cls_secondary_handles = "0x0011,0x0012";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<Nothing> schedRegistered;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&schedRegistered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(schedRegistered);

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Create a task to be launched in the mesos-container.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  // Task is ready. Verify `ContainerStatus` is present in slave state.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Object> netCls = parse->find<JSON::Object>(
      "frameworks[0].executors[0].tasks[0].statuses[0]."
      "container_status.cgroup_info.net_cls");

  ASSERT_SOME(netCls);

  uint32_t classid =
    netCls->values["classid"].as<JSON::Number>().as<uint32_t>();

  // Check the primary and the secondary handle.
  EXPECT_EQ(0x0012u, classid >> 16);
  EXPECT_EQ(0x0011u, classid & 0xffff);

  driver.stop();
  driver.join();
}


TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_PERF_Sample)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);
  flags.isolation = "cgroups/perf_event";

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 120");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  // This first sample is likely to be empty because perf hasn't
  // completed yet but we should still have the required fields.
  Future<ResourceStatistics> statistics1 = containerizer->usage(containerId);
  AWAIT_READY(statistics1);
  ASSERT_TRUE(statistics1->has_perf());
  EXPECT_TRUE(statistics1->perf().has_timestamp());
  EXPECT_TRUE(statistics1->perf().has_duration());

  // Wait until we get the next sample. We use a generous timeout of
  // two seconds because we currently have a one second reap interval;
  // when running perf with perf_duration of 250ms we won't notice the
  // exit for up to one second.
  ResourceStatistics statistics2;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> statistics = containerizer->usage(containerId);
    AWAIT_READY(statistics);

    statistics2 = statistics.get();

    ASSERT_TRUE(statistics2.has_perf());

    if (statistics1->perf().timestamp() !=
        statistics2.perf().timestamp()) {
      break;
    }

    os::sleep(Milliseconds(250));
    waited += Milliseconds(250);
  } while (waited < Seconds(2));

  EXPECT_NE(statistics1->perf().timestamp(),
            statistics2.perf().timestamp());

  EXPECT_TRUE(statistics2.perf().has_cycles());
  EXPECT_LE(0u, statistics2.perf().cycles());

  EXPECT_TRUE(statistics2.perf().has_task_clock());
  EXPECT_LE(0.0, statistics2.perf().task_clock());

  driver.stop();
  driver.join();
}


// Test that the perf event subsystem can be enabled after the agent
// restart. Previously created containers will not report perf
// statistics but newly created containers will.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_PERF_PerfForward)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent using a containerizer without the perf_event isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<slave::Containerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Future<TaskStatus> statusStarting1;
  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillRepeatedly(Return());

  TaskInfo task1 = createTask(
      offers1.get()[0].slave_id(),
      Resources::parse("cpus:0.5;mem:128").get(),
      "sleep 1000");

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(statusStarting1);
  EXPECT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers->size());

  ContainerID containerId1 = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  // There should not be any perf statistics.
  EXPECT_FALSE(usage->has_perf());

  slave.get()->terminate();

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Start a slave using a containerizer with the perf_event isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem,cgroups/perf_event";
  flags.perf_events = "cycles,task-clock";
  flags.perf_duration = Milliseconds(250);
  flags.perf_interval = Milliseconds(500);

  containerizer.reset();

  create = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(create);

  containerizer.reset(create.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  AWAIT_READY(__recover);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // The first container should not report any perf statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_FALSE(usage->has_perf());

  // Start a new container which will start reporting perf statistics.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting2;
  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting2))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(statusStarting2);
  EXPECT_EQ(TASK_STARTING, statusStarting2->state());

  AWAIT_READY(statusRunning2);
  EXPECT_EQ(TASK_RUNNING, statusRunning2->state());

  containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(2u, containers->size());
  EXPECT_TRUE(containers->contains(containerId1));

  ContainerID containerId2;
  foreach (const ContainerID& containerId, containers.get()) {
    if (containerId != containerId1) {
      containerId2 = containerId;
    }
  }

  usage = containerizer->usage(containerId2);
  AWAIT_READY(usage);

  EXPECT_TRUE(usage->has_perf());

  // TODO(jieyu): Consider kill the perf process.

  driver.stop();
  driver.join();
}


// Test that the memory subsystem can be enabled after the agent
// restart. Previously created containers will not perform memory
// isolation but newly created containers will.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_MemoryForward)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent using a containerizer without the memory isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<slave::Containerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Future<TaskStatus> statusStarting1;
  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillRepeatedly(Return());

  TaskInfo task1 = createTask(
      offers1.get()[0].slave_id(),
      Resources::parse("cpus:0.5;mem:128").get(),
      "sleep 1000");

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(statusStarting1);
  EXPECT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers->size());

  ContainerID containerId1 = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  // There should not be any memory statistics.
  EXPECT_FALSE(usage->has_mem_total_bytes());

  slave.get()->terminate();

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Start an agent using a containerizer with the memory isolation.
  flags.isolation = "cgroups/cpu,cgroups/mem";

  containerizer.reset();

  create = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(create);

  containerizer.reset(create.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until agent recovery is complete.
  AWAIT_READY(__recover);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // The first container should not report memory statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_FALSE(usage->has_mem_total_bytes());

  // Start a new container which will start reporting memory statistics.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting2;
  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting2))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(statusStarting2);
  EXPECT_EQ(TASK_STARTING, statusStarting2->state());

  AWAIT_READY(statusRunning2);
  EXPECT_EQ(TASK_RUNNING, statusRunning2->state());

  containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(2u, containers->size());
  EXPECT_TRUE(containers->contains(containerId1));

  ContainerID containerId2;
  foreach (const ContainerID& containerId, containers.get()) {
    if (containerId != containerId1) {
      containerId2 = containerId;
    }
  }

  usage = containerizer->usage(containerId2);
  AWAIT_READY(usage);

  EXPECT_TRUE(usage->has_mem_total_bytes());

  driver.stop();
  driver.join();
}


// Test that the memory subsystem can be disabled after the agent
// restart. Previously created containers will perform memory isolation
// but newly created containers will.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_MemoryBackward)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start an agent using a containerizer with the memory isolation.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/cpu,cgroups/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<slave::Containerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Future<TaskStatus> statusStarting1;
  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillRepeatedly(Return());

  TaskInfo task1 = createTask(
      offers1.get()[0].slave_id(),
      Resources::parse("cpus:0.5;mem:128").get(),
      "sleep 1000");

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(statusStarting1);
  EXPECT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  EXPECT_EQ(1u, containers->size());

  ContainerID containerId1 = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_TRUE(usage->has_mem_total_bytes());

  slave.get()->terminate();

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Start an agent using a containerizer without the memory isolation.
  flags.isolation = "cgroups/cpu";

  containerizer.reset();

  create = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(create);

  containerizer.reset(create.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until agent recovery is complete.
  AWAIT_READY(__recover);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // The first container should not report memory statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  // After restart the agent without the memory isolation,
  // the container should not report memory statistics.
  EXPECT_FALSE(usage->has_mem_total_bytes());

  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting2;
  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting2))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(statusStarting2);
  EXPECT_EQ(TASK_STARTING, statusStarting2->state());

  AWAIT_READY(statusRunning2);
  EXPECT_EQ(TASK_RUNNING, statusRunning2->state());

  containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(2u, containers->size());
  EXPECT_TRUE(containers->contains(containerId1));

  ContainerID containerId2;
  foreach (const ContainerID& containerId, containers.get()) {
    if (containerId != containerId1) {
      containerId2 = containerId;
    }
  }

  usage = containerizer->usage(containerId2);
  AWAIT_READY(usage);

  // After restart the agent without the memory isolation,
  // the container should not report memory statistics.
  EXPECT_FALSE(usage->has_mem_total_bytes());

  driver.stop();
  driver.join();
}


// This test verifies the cgroups blkio statistics
// of the container can be successfully retrieved.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_BlkioUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/blkio";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Create a task to generate a 10k file with 10 disk writes.
  //
  // TODO(qianzhang): In some old platforms (CentOS 6 and Ubuntu 14),
  // the first disk write of a blkio cgroup will always be missed in
  // the blkio throttling statistics, so here we run two `dd` commands,
  // the first one which does the first disk write will be missed on
  // those platforms, and the second one will be recorded in the blkio
  // throttling statistics. When we drop the CentOS 6 and Ubuntu 14
  // support, we should remove the first `dd` command.
  TaskInfo task = createTask(
      offers.get()[0],
      "dd if=/dev/zero of=file bs=1024 count=1 oflag=dsync && "
      "dd if=/dev/zero of=file bs=1024 count=10 oflag=dsync && "
      "sleep 1000");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // NOTE: The command executor's id is the same as the task id.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  const string directory = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      offers.get()[0].slave_id(),
      offers.get()[0].framework_id(),
      executorId);

  ASSERT_TRUE(os::exists(directory));

  // Make sure the file is completely generated.
  const string filePath = path::join(directory, "file");
  Option<Bytes> fileSize;
  Duration waited = Duration::zero();

  do {
    if (os::exists(filePath)) {
      Try<Bytes> size = os::stat::size(filePath);
      ASSERT_SOME(size);

      if (size->bytes() == 10240) {
        fileSize = size.get();
        break;
      }
    }

    os::sleep(Seconds(1));
    waited += Seconds(1);
  } while (waited < process::TEST_AWAIT_TIMEOUT);

  ASSERT_SOME(fileSize);
  ASSERT_EQ(10240u, fileSize->bytes());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // We only check throttling statistics but not cfq statistics, because
  // in the environment where the disk IO scheduler is not cfq, all the
  // cfq statistics may be 0. And there must be at least two entries in
  // the throttling statistics, one is the total statistics, the others
  // are device specific statistics.
  ASSERT_TRUE(usage->has_blkio_statistics());
  EXPECT_LE(2, usage->blkio_statistics().throttling_size());

  // We only check the total throttling statistics.
  Option<CgroupInfo::Blkio::Throttling::Statistics> totalThrottling;
  foreach (const CgroupInfo::Blkio::Throttling::Statistics& statistics,
           usage->blkio_statistics().throttling()) {
    if (!statistics.has_device()) {
      totalThrottling = statistics;
    }
  }

  EXPECT_SOME(totalThrottling);
  EXPECT_EQ(1, totalThrottling->io_serviced_size());
  EXPECT_EQ(1, totalThrottling->io_service_bytes_size());

  const CgroupInfo::Blkio::Value& totalIOServiced =
      totalThrottling->io_serviced(0);

  EXPECT_TRUE(totalIOServiced.has_op());
  EXPECT_EQ(CgroupInfo::Blkio::TOTAL, totalIOServiced.op());
  EXPECT_TRUE(totalIOServiced.has_value());
  EXPECT_LE(10u, totalIOServiced.value());

  const CgroupInfo::Blkio::Value& totalIOServiceBytes =
      totalThrottling->io_service_bytes(0);

  EXPECT_TRUE(totalIOServiceBytes.has_op());
  EXPECT_EQ(CgroupInfo::Blkio::TOTAL, totalIOServiceBytes.op());
  EXPECT_TRUE(totalIOServiceBytes.has_value());
  EXPECT_LE(10240u, totalIOServiceBytes.value());

  driver.stop();
  driver.join();
}


// This test verifies all the local enabled cgroups subsystems
// can be automatically loaded by the cgroup isolator.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_AutoLoadSubsystems)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "cgroups/all";

  Fetcher fetcher(flags);

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the update to verify that the task has been launched.
  AWAIT_READY(statusStarting);
  ASSERT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning->state());

  // Task is ready. Make sure there is exactly 1 container in the hashset.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerId = *(containers->begin());

  Try<set<string>> enabledSubsystems = cgroups::subsystems();
  ASSERT_SOME(enabledSubsystems);

  set<string> supportedSubsystems = {
    CGROUP_SUBSYSTEM_BLKIO_NAME,
    CGROUP_SUBSYSTEM_CPU_NAME,
    CGROUP_SUBSYSTEM_CPUACCT_NAME,
    CGROUP_SUBSYSTEM_CPUSET_NAME,
    CGROUP_SUBSYSTEM_DEVICES_NAME,
    CGROUP_SUBSYSTEM_HUGETLB_NAME,
    CGROUP_SUBSYSTEM_MEMORY_NAME,
    CGROUP_SUBSYSTEM_NET_CLS_NAME,
    CGROUP_SUBSYSTEM_NET_PRIO_NAME,
    CGROUP_SUBSYSTEM_PERF_EVENT_NAME,
    CGROUP_SUBSYSTEM_PIDS_NAME,
  };

  // Check cgroups for all the local enabled subsystems
  // have been created for the container.
  foreach (const string& subsystem, enabledSubsystems.get()) {
    if (supportedSubsystems.count(subsystem) == 0) {
      continue;
    }

    Result<string> hierarchy = cgroups::hierarchy(subsystem);
    ASSERT_SOME(hierarchy);

    string cgroup = path::join(flags.cgroups_root, containerId.value());

    ASSERT_TRUE(os::exists(path::join(hierarchy.get(), cgroup)));
  }

  driver.stop();
  driver.join();
}


// This test verifies that after the agent recovery/upgrade, nested
// containers could still be launched under old containers which
// were launched before agent restarts if there are new cgroup
// subsystems are added in the agent cgroup isolation.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_AgentRecoveryWithNewCgroupSubsystems)
{
  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime,cgroups/mem";
  flags.image_providers = "docker";
  flags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer1 = offers1->offers(0);

  v1::TaskInfo taskInfo1 = v1::createTask(
      offer1.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "sleep 1000");

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate1),
        v1::scheduler::SendAcknowledge(frameworkId, offer1.agent_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate1),
        v1::scheduler::SendAcknowledge(frameworkId, offer1.agent_id())))
    .WillRepeatedly(Return());

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer1,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1}))}));

  AWAIT_READY(startingUpdate1);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate1->status().state());
  ASSERT_EQ(taskInfo1.task_id(), startingUpdate1->status().task_id());

  AWAIT_READY(runningUpdate1);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate1->status().state());
  ASSERT_EQ(taskInfo1.task_id(), runningUpdate1->status().task_id());

  slave.get()->terminate();
  slave->reset();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Update the cgroup isolation to introduce new subsystems.
  flags.isolation = "filesystem/linux,docker/runtime,cgroups/all";
  slave = this->StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  Future<Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  v1::TaskInfo taskInfo2 = v1::createTask(
      offer2.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "sleep 1000");

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate2),
        v1::scheduler::SendAcknowledge(frameworkId, offer2.agent_id())))
    .WillOnce(FutureArg<1>(&runningUpdate2));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer2,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo2}))}));

  AWAIT_READY(startingUpdate2);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate2->status().state());
  ASSERT_EQ(taskInfo2.task_id(), startingUpdate2->status().task_id());

  AWAIT_READY(runningUpdate2);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate2->status().state());
  ASSERT_EQ(taskInfo2.task_id(), runningUpdate2->status().task_id());
}


// This test verifies the container-specific cgroups are correctly mounted
// inside the nested container.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_NestedContainerSpecificCgroupsMount)
{
  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime,cgroups/mem,cgroups/cpu";
  flags.image_providers = "docker";
  flags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Create a task to check if its memory and CPU shares (including both
  // executor's and task's) are correctly set in its specific cgroup.
  //
  // And we also verify the freezer cgroup is correctly mounted for this task
  // by checking if the current shell PID is included in the freezer cgroup.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "test `cat /sys/fs/cgroup/memory/memory.soft_limit_in_bytes` = 67108864 "
      "&& test `cat /sys/fs/cgroup/cpu/cpu.shares` = 204"
      "&& grep $$ /sys/fs/cgroup/freezer/cgroup.procs");

  mesos::v1::Image image;
  image.set_type(mesos::v1::Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  mesos::v1::ContainerInfo* container = taskInfo.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, offer.agent_id())));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);
  ASSERT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());

  AWAIT_READY(runningUpdate);
  ASSERT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());

  AWAIT_READY(finishedUpdate);
  ASSERT_EQ(v1::TASK_FINISHED, finishedUpdate->status().state());
  ASSERT_EQ(taskInfo.task_id(), finishedUpdate->status().task_id());
}


// This test verifies the container-specific cgroups are correctly mounted for
// the command task.
TEST_F(CgroupsIsolatorTest, ROOT_CGROUPS_CommandTaskSpecificCgroupsMount)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime,cgroups/mem,cgroups/cpu";
  flags.image_providers = "docker";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers->size());

  // Create a task to check if its memory and CPU shares (including both
  // executor's and task's) are correctly set in its specific cgroup.
  //
  // And we also verify the freezer cgroup is correctly mounted for this task
  // by checking if the current shell PID is included in the freezer cgroup.
  TaskInfo task = createTask(
      offers->front().slave_id(),
      Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      "test `cat /sys/fs/cgroup/memory/memory.soft_limit_in_bytes` = 67108864 "
      "&& test `cat /sys/fs/cgroup/cpu/cpu.shares` = 204"
      "&& grep $$ /sys/fs/cgroup/freezer/cgroup.procs");

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
