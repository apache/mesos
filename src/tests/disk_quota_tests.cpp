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

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/fs.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/isolators/posix/disk.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#if __linux__
#include "tests/containerizer/docker_archive.hpp"
#endif

using namespace process;

using std::string;
using std::vector;

using testing::_;
using testing::Return;

using mesos::internal::master::Master;

using mesos::internal::slave::DiskUsageCollector;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

namespace mesos {
namespace internal {
namespace tests {


class DiskUsageCollectorTest : public TemporaryDirectoryTest {};


// TODO(jieyu): Consider adding a test to verify that minimal check
// interval is honored.


// This test verifies the usage of a file.
TEST_F(DiskUsageCollectorTest, File)
{
  // Create a file and write 8K bytes.
  string path = path::join(os::getcwd(), "file");
  ASSERT_SOME(os::write(path, string(Kilobytes(8).bytes(), 'x')));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage = collector.usage(path, {});
  AWAIT_READY(usage);

  // NOTE: A typical file system needs more disk space to keep meta
  // data. So the check here is not a strict equal-to check.
  EXPECT_GE(usage.get(), Kilobytes(8));
}


// This test verifies the usage of a directory.
TEST_F(DiskUsageCollectorTest, Directory)
{
  // Create files and subdirectories in the working directory.
  string file1 = path::join(os::getcwd(), "file1");
  string file2 = path::join(os::getcwd(), "file2");

  string dir = path::join(os::getcwd(), "dir");
  string file3 = path::join(dir, "file3");
  string file4 = path::join(dir, "file4");

  ASSERT_SOME(os::mkdir(dir));

  ASSERT_SOME(os::write(file1, string(Kilobytes(8).bytes(), 'x')));
  ASSERT_SOME(os::write(file2, string(Kilobytes(4).bytes(), 'y')));
  ASSERT_SOME(os::write(file3, string(Kilobytes(1).bytes(), 'z')));
  ASSERT_SOME(os::write(file4, string(Kilobytes(2).bytes(), '1')));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage = collector.usage(os::getcwd(), {});
  AWAIT_READY(usage);

  EXPECT_GE(usage.get(), Kilobytes(15));
}


// This test verifies that symbolic links are not followed.
TEST_F(DiskUsageCollectorTest, SymbolicLink)
{
  string file = path::join(os::getcwd(), "file");
  ASSERT_SOME(os::write(file, string(Kilobytes(64).bytes(), 'x')));

  // Create a symbolic link to the current directory.
  string link = path::join(os::getcwd(), "link");
  ASSERT_SOME(fs::symlink(os::getcwd(), link));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage1 = collector.usage(os::getcwd(), {});
  Future<Bytes> usage2 = collector.usage(link, {});

  AWAIT_READY(usage1);
  EXPECT_GE(usage1.get(), Kilobytes(64));
  EXPECT_LT(usage1.get(), Kilobytes(128));

  AWAIT_READY(usage2);
  EXPECT_LT(usage2.get(), Kilobytes(64));
}


#ifdef __linux__
// This test verifies that relative exclude paths work and that
// absolute ones don't (in cases when the directory path itself
// is relative).
TEST_F(DiskUsageCollectorTest, ExcludeRelativePath)
{
  string file = path::join(os::getcwd(), "file");

  // Create a 128k file.
  ASSERT_SOME(os::write(file, string(Kilobytes(128).bytes(), 'x')));

  DiskUsageCollector collector(Milliseconds(1));

  // Exclude 'file' and make sure the usage is way below 128k.
  Future<Bytes> usage1 = collector.usage(os::getcwd(), {"file"});
  AWAIT_READY(usage1);
  EXPECT_LT(usage1.get(), Kilobytes(64));

  // Exclude absolute path of 'file' with the relative directory
  // path. Pattern matching is expected to fail causing exclude
  // to have no effect.
  Future<Bytes> usage2 = collector.usage(".", {file});
  EXPECT_GE(usage2.get(), Kilobytes(128));
}
#endif


class DiskQuotaTest : public MesosTest {};


class DiskQuotaEnforcement
  : public DiskQuotaTest,
    public ::testing::WithParamInterface<ParamDiskQuota::Type>
{
public:
  DiskQuotaEnforcement() :DiskQuotaTest() {}

  static Future<Nothing> CreateDockerImage(
      const string& registry, const string& name)
  {
#if __linux__
    return DockerArchive::create(registry, name);
#else
    return Failure("DockerArchive is only supported on Linux");
#endif
  }
};


INSTANTIATE_TEST_CASE_P(
    Enforcing,
    DiskQuotaEnforcement,
    ::testing::ValuesIn(ParamDiskQuota::parameters()),
    ParamDiskQuota::Printer());


// This test verifies that the container will be killed if the disk
// usage exceeds its quota.
TEST_P(DiskQuotaEnforcement, DiskUsageExceedsQuota)
{
  // NOTE: We don't use the "ROOT_" tag on the test because that
  // would require the SANDBOX tests to be run as root as well.
  // If we did that, then we would lose disk isolator test coverage
  // in the default CI configuration.
  if (GetParam() == ParamDiskQuota::ROOTFS && ::geteuid() != 0) {
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem,disk/du";

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  flags.container_disk_watch_interval = Milliseconds(1);
  flags.enforce_container_disk_quota = true;

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.isolation += ",filesystem/linux,docker/runtime";

    flags.docker_registry = path::join(sandbox.get(), "registry");
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(CreateDockerImage(flags.docker_registry, "test_image"));
  }

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task;

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  switch (GetParam()) {
    case ParamDiskQuota::SANDBOX:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:1").get(),
        "dd if=/dev/zero of=file bs=1048576 count=2 && sleep 1000");
      break;
    case ParamDiskQuota::ROOTFS:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:1").get(),
        "dd if=/dev/zero of=/tmp/file bs=1048576 count=2 && sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
  }

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_FAILED, status2->state());

  driver.stop();
  driver.join();
}


// This test verifies that the container will be killed if the volume
// usage exceeds its quota.
TEST_F(DiskQuotaTest, VolumeUsageExceedsQuota)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "posix/cpu,posix/mem,disk/du";

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  slaveFlags.container_disk_watch_interval = Milliseconds(1);
  slaveFlags.enforce_container_disk_quota = true;
  slaveFlags.resources = "cpus:2;mem:128;disk(role1):128";

  Try<Resources> initialResources =
    Resources::parse(slaveFlags.resources.get());
  ASSERT_SOME(initialResources);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task that requests a 1 MB persistent volume but attempts
  // to use 2MB.
  Resources volume = createPersistentVolume(
      Megabytes(1),
      "role1",
      "id1",
      "volume_path",
      None(),
      None(),
      frameworkInfo.principal());

  // We intentionally request a sandbox that is much bigger (16MB) than
  // the file the task writes (2MB) to the persistent volume (1MB). This
  // makes sure that the quota is indeed enforced on the persistent volume.
  Resources taskResources =
    Resources::parse("cpus:1;mem:64;disk(role1):16").get() + volume;

  TaskInfo task = createTask(
      offer.slave_id(),
      taskResources,
      "dd if=/dev/zero of=volume_path/file bs=1048576 count=2 && sleep 1000");

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  // Create the volume and launch the task.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume),
      LAUNCH({task})});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_FAILED, status2->state());

  driver.stop();
  driver.join();
}


// This test verifies that the container will not be killed if
// disk_enforce_quota flag is false (even if the disk usage exceeds
// its quota).
TEST_P(DiskQuotaEnforcement, NoQuotaEnforcement)
{
  if (GetParam() == ParamDiskQuota::ROOTFS && ::geteuid() != 0) {
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem,disk/du";

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  flags.container_disk_watch_interval = Milliseconds(1);
  flags.enforce_container_disk_quota = false;

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.isolation += ",filesystem/linux,docker/runtime";

    flags.docker_registry = path::join(sandbox.get(), "registry");
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(CreateDockerImage(flags.docker_registry, "test_image"));
  }

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

  const Offer& offer = offers.get()[0];

  TaskInfo task;

  // Create a task that uses 2MB disk.
  switch (GetParam()) {
    case ParamDiskQuota::SANDBOX:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:1").get(),
        "dd if=/dev/zero of=file bs=1048576 count=2 && sleep 1000");
      break;
    case ParamDiskQuota::ROOTFS:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:1").get(),
        "dd if=/dev/zero of=/tmp/file bs=1048576 count=2 && sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
  }

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerId = *(containers->begin());

  // Wait until disk usage can be retrieved and the usage actually
  // exceeds the limit. If the container is killed due to quota
  // enforcement (which shouldn't happen), the 'usage' call will
  // return a failed future, leading to a failed test.
  Duration elapsed = Duration::zero();
  while (true) {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    EXPECT_EQ(Megabytes(1), Bytes(usage->disk_limit_bytes()));

    if (usage->has_disk_used_bytes() &&
        usage->disk_used_bytes() > usage->disk_limit_bytes()) {
      break;
    }

    ASSERT_LT(elapsed, Seconds(5));

    os::sleep(Milliseconds(1));
    elapsed += Milliseconds(1);
  }

  driver.stop();
  driver.join();
}


TEST_P(DiskQuotaEnforcement, ResourceStatistics)
{
  if (GetParam() == ParamDiskQuota::ROOTFS && ::geteuid() != 0) {
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem,disk/du";

  flags.resources = strings::format("disk(%s):10", DEFAULT_TEST_ROLE).get();

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  flags.container_disk_watch_interval = Milliseconds(1);

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.isolation += ",filesystem/linux,docker/runtime";

    flags.docker_registry = path::join(sandbox.get(), "registry");
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(CreateDockerImage(flags.docker_registry, "test_image"));
  }

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  Resource volume = createPersistentVolume(
      Megabytes(4),
      DEFAULT_TEST_ROLE,
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  Resources taskResources = Resources::parse("cpus:1;mem:128").get();

  taskResources += createDiskResource(
      "3",
      DEFAULT_TEST_ROLE,
      None(),
      None());

  taskResources += volume;

  TaskInfo task;

  // Create a task that uses 2MB disk.
  switch (GetParam()) {
    case ParamDiskQuota::SANDBOX:
      task = createTask(
        offer.slave_id(),
        taskResources,
        "dd if=/dev/zero of=file bs=1048576 count=2 && "
        "dd if=/dev/zero of=path1/file bs=1048576 count=2 && "
        "sleep 1000");
      break;
    case ParamDiskQuota::ROOTFS:
      task = createTask(
        offer.slave_id(),
        taskResources,
        "dd if=/dev/zero of=/tmp/file bs=1048576 count=2 && "
        "dd if=/dev/zero of=path1/file bs=1048576 count=2 && "
        "sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
  }

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.acceptOffers(
      {offer.id()},
      {CREATE(volume),
       LAUNCH({task})});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerId = *(containers->begin());

  // Wait until disk usage can be retrieved.
  Duration elapsed = Duration::zero();
  while (true) {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    EXPECT_EQ(Megabytes(3), Bytes(usage->disk_limit_bytes()));

    if (usage->has_disk_used_bytes()) {
      EXPECT_LE(usage->disk_used_bytes(), usage->disk_limit_bytes());
    }

    ASSERT_EQ(2, usage->disk_statistics().size());

    bool done = true;
    foreach (const DiskStatistics& statistics, usage->disk_statistics()) {
      ASSERT_TRUE(statistics.has_limit_bytes());
      EXPECT_EQ(
          statistics.has_persistence() ? Megabytes(4) : Megabytes(3),
          statistics.limit_bytes());

      if (!statistics.has_used_bytes()) {
        done = false;
      } else {
        EXPECT_GT(
            statistics.has_persistence() ? Megabytes(4) : Megabytes(3),
            statistics.used_bytes());
      }
    }

    if (done) {
      break;
    }

    ASSERT_LT(elapsed, Seconds(5));

    os::sleep(Milliseconds(1));
    elapsed += Milliseconds(1);
  }

  driver.killTask(task.task_id());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_KILLED, status2->state());

  driver.stop();
  driver.join();
}


// This test verifies that disk quota isolator recovers properly after
// the slave restarts.
TEST_P(DiskQuotaEnforcement, SlaveRecovery)
{
  if (GetParam() == ParamDiskQuota::ROOTFS && ::geteuid() != 0) {
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem,disk/du";
  flags.container_disk_watch_interval = Milliseconds(1);

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.isolation += ",filesystem/linux,docker/runtime";

    flags.docker_registry = path::join(sandbox.get(), "registry");
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(CreateDockerImage(flags.docker_registry, "test_image"));
  }

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

  const Offer& offer = offers.get()[0];

  TaskInfo task;

  // Create a task that uses 2MB disk.
  switch (GetParam()) {
    case ParamDiskQuota::SANDBOX:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:3").get(),
        "dd if=/dev/zero of=file bs=1048576 count=2 && sleep 1000");
      break;
    case ParamDiskQuota::ROOTFS:
      task = createTask(
        offer.slave_id(),
        Resources::parse("cpus:1;mem:128;disk:3").get(),
        "dd if=/dev/zero of=/tmp/file bs=1048576 count=2 && sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
  }

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  const ContainerID& containerId = *(containers->begin());

  // Stop the slave.
  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  containerizer.reset(_containerizer.get());

  detector = master.get()->createDetector();

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  AWAIT_READY(_recover);

  // Wait for slave to schedule reregister timeout.
  Clock::settle();

  // Ensure the executor reregisters before completing recovery.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);

  // NOTE: We resume the clock because we need the reaper to reap the
  // 'du' subprocess.
  Clock::resume();

  // Wait until disk usage can be retrieved.
  Duration elapsed = Duration::zero();
  while (true) {
    Future<ResourceStatistics> usage = containerizer->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    EXPECT_EQ(Megabytes(3), Bytes(usage->disk_limit_bytes()));

    if (usage->has_disk_used_bytes()) {
      EXPECT_LE(usage->disk_used_bytes(), usage->disk_limit_bytes());

      // NOTE: This is to capture the regression in MESOS-2452. The data
      // stored in the executor meta directory should be less than 64K.
      if (usage->disk_used_bytes() > Kilobytes(64).bytes()) {
        break;
      }
    }

    ASSERT_LT(elapsed, process::TEST_AWAIT_TIMEOUT);

    os::sleep(Milliseconds(1));
    elapsed += Milliseconds(1);
  }

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
