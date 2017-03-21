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

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/format.hpp>
#include <stout/gtest.hpp>

#include <mesos/v1/scheduler.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.hpp"

#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"
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

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::NetClsHandle;
using mesos::internal::slave::NetClsHandleManager;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Event;

using process::Future;
using process::Owned;
using process::Queue;

using process::http::OK;
using process::http::Response;

using std::set;
using std::string;
using std::vector;

using testing::_;
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

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
    EXPECT_NE(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'echo $$ > %s'",
        path::join(hierarchy.get(), cgroup, "cgroup.procs")).get()));

    // Verify that the user can create a cgroup under the container's
    // cgroup as the isolator changes the owner of the cgroup.
    string userCgroup = path::join(cgroup, "user");

    EXPECT_EQ(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'mkdir %s'",
        path::join(hierarchy.get(), userCgroup)).get()));

    // Verify that the user can manipulate control files in the
    // created cgroup as it's owned by the user.
    EXPECT_EQ(0, os::system(strings::format(
        "su - nobody -s /bin/sh -c 'echo $$ > %s'",
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1->size());
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
  EXPECT_NE(0u, offers2->size());
  EXPECT_EQ(allocatedResources(cpus, frameworkInfo.role()),
            Resources(offers2.get()[0].resources()));

  TaskInfo task = createTask(
      offers2.get()[0].slave_id(),
      cpus,
      "sleep 120");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers2.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  string cpuCgroup= path::join(flags.cgroups_root, containerId.value());

  double totalCpus = cpus.cpus().get() + DEFAULT_EXECUTOR_CPUS;
  EXPECT_SOME_EQ(
      CPU_SHARES_PER_CPU_REVOCABLE * totalCpus,
      cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup));

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

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
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Future<ResourceStatistics> usage = containerizer->usage(containerId);
  AWAIT_READY(usage);

  // Expect that no more than 300 ms of cpu time has been consumed. We
  // also check that at least 50 ms of cpu time has been consumed so
  // this test will fail if the host system is very heavily loaded.
  // This behavior is correct because under such conditions we aren't
  // actually testing the CFS cpu limiter.
  double cpuTime = usage->cpus_system_time_secs() +
                   usage->cpus_user_time_secs();

  EXPECT_GE(0.30, cpuTime);
  EXPECT_LE(0.05, cpuTime);

  driver.stop();
  driver.join();
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

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(DoAll(v1::scheduler::SendSubscribe(frameworkInfo),
                    FutureSatisfy(&connected)));

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

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
  EXPECT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo("ls", {"ls", "-al", "/"}));

  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

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

  Fetcher fetcher;

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

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Capture the update to verify that the task has been launched.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

  // Task is ready.  Make sure there is exactly 1 container in the hashset.
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

  driver.killTask(status->task_id());

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

  Fetcher fetcher;

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

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status->state());

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  TaskInfo task = createTask(offers.get()[0], "sleep 120");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

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

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers1->size());

  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
  EXPECT_NE(0u, offers2->size());

  // The first container should not report any perf statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_FALSE(usage->has_perf());

  // Start a new container which will start reporting perf statistics.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

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

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers1->size());

  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
  EXPECT_NE(0u, offers2->size());

  // The first container should not report memory statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  EXPECT_FALSE(usage->has_mem_total_bytes());

  // Start a new container which will start reporting memory statistics.
  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

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

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers1->size());

  Future<TaskStatus> statusRunning1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
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
  EXPECT_NE(0u, offers2->size());

  // The first container should not report memory statistics.
  usage = containerizer->usage(containerId1);
  AWAIT_READY(usage);

  // After restart the agent without the memory isolation,
  // the container should not report memory statistics.
  EXPECT_FALSE(usage->has_mem_total_bytes());

  TaskInfo task2 = createTask(offers2.get()[0], "sleep 1000");

  Future<TaskStatus> statusRunning2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.launchTasks(offers2.get()[0].id(), {task2});

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
