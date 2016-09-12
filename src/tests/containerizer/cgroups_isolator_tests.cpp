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
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using std::string;
using std::vector;

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
