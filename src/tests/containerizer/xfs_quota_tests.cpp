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

#include <linux/loop.h>

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/gtest.hpp>
#include <process/pid.hpp>

#include <stout/fs.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "linux/fs.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolators/xfs/disk.hpp"
#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::xfs;

using namespace process;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::Return;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;
using mesos::internal::slave::XfsDiskIsolatorProcess;

using mesos::master::detector::MasterDetector;

namespace mesos {
namespace internal {
namespace tests {

static QuotaInfo makeQuotaInfo(
    Bytes limit,
    Bytes used)
{
  return {limit, limit, used};
}


class ROOT_XFS_TestBase : public MesosTest
{
public:
  ROOT_XFS_TestBase(
      const Option<std::string>& _mountOptions = None(),
      const Option<std::string>& _mkfsOptions = None())
    : mountOptions(_mountOptions), mkfsOptions(_mkfsOptions) {}

  virtual void SetUp()
  {
    MesosTest::SetUp();

    Try<string> base = environment->mkdtemp();
    ASSERT_SOME(base) << "Failed to mkdtemp";

    string devPath = path::join(base.get(), "device");
    string mntPath = path::join(base.get(), "mnt");

    ASSERT_SOME(os::mkdir(mntPath));
    ASSERT_SOME(mkfile(devPath, Megabytes(40)));

    // Get an unused loop device.
    Try<string> loop = mkloop();
    ASSERT_SOME(loop);

    // Attach the loop to a backing file.
    Try<Subprocess> losetup = subprocess(
        "losetup " + loop.get() + " " + devPath,
        Subprocess::PATH(os::DEV_NULL));

    ASSERT_SOME(losetup);
    AWAIT_READY(losetup->status());
    ASSERT_SOME_EQ(0, losetup->status().get());

    loopDevice = loop.get();
    ASSERT_SOME(loopDevice);

    // Make an XFS filesystem (using the force flag). The defaults
    // should be good enough for tests.
    Try<Subprocess> mkfs = subprocess(
        "mkfs.xfs -f " +
        mkfsOptions.getOrElse("") +
        " " +
        loopDevice.get(),
        Subprocess::PATH(os::DEV_NULL));

    ASSERT_SOME(mkfs);
    AWAIT_READY(mkfs->status());
    ASSERT_SOME_EQ(0, mkfs->status().get());

    ASSERT_SOME(fs::mount(
        loopDevice.get(),
        mntPath,
        "xfs",
        0, // Flags.
        mountOptions.getOrElse("")));
    mountPoint = mntPath;

    ASSERT_SOME(os::chdir(mountPoint.get()))
      << "Failed to chdir into '" << mountPoint.get() << "'";
  }

  virtual void TearDown()
  {
    if (mountPoint.isSome()) {
      fs::unmount(mountPoint.get(), MNT_FORCE | MNT_DETACH);
    }

    // Make sure we resume the clock so that we can wait on the
    // `losetup` process.
    if (Clock::paused()) {
      Clock::resume();
    }

    // Make a best effort to tear everything down. We don't make any assertions
    // here because even if something goes wrong we still want to clean up as
    // much as we can.
    if (loopDevice.isSome()) {
      Try<Subprocess> cmdProcess = subprocess(
          "losetup -d " + loopDevice.get(),
          Subprocess::PATH(os::DEV_NULL));

      if (cmdProcess.isSome()) {
        cmdProcess->status().await(process::TEST_AWAIT_TIMEOUT);
      }
    }

    MesosTest::TearDown();
  }

  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

    // We only need an XFS-specific directory for the work directory. We
    // don't mind that other flags refer to a different temp directory.
    flags.work_dir = mountPoint.get();
    flags.isolation = "disk/xfs";
    flags.enforce_container_disk_quota = true;
    return flags;
  }

  static Try<Nothing> mkfile(string path, Bytes size)
  {
    Try<int> fd = os::open(path, O_CREAT | O_RDWR | O_EXCL);

    if (fd.isError()) {
      return Error(fd.error());
    }

    // XFS supports posix_fallocate(3), and we depend on it actually
    // allocating storage in the quota tests.
    if (int error = ::posix_fallocate(fd.get(), 0, size.bytes())) {
      os::close(fd.get());
      return Error("posix_fallocate failed: " + os::strerror(error));
    }

    os::close(fd.get());
    return Nothing();
  }

  static Try<string> mkloop()
  {
    Try<int> fd = os::open("/dev/loop-control", O_RDWR);

    if (fd.isError()) {
      return Error(fd.error());
    }

    // All failure cases here are reported in errno with a -1 return value.
    int devno = ::ioctl(fd.get(), LOOP_CTL_GET_FREE);
    if (devno == -1) {
      ErrnoError error("ioctl(LOOP_CTL_GET_FREE failed");
      os::close(fd.get());
      return error;
    }

    os::close(fd.get());

    return string("/dev/loop") + stringify(devno);
  }

  Try<list<string>> getSandboxes()
  {
    return os::glob(path::join(
      slave::paths::getSandboxRootDir(mountPoint.get()),
      "*",
      "frameworks",
      "*",
      "executors",
      "*",
      "runs",
      "*"));
  }

  Option<string> mountOptions;
  Option<string> mkfsOptions;
  Option<string> loopDevice; // The loop device we attached.
  Option<string> mountPoint; // XFS filesystem mountpoint.
};


// ROOT_XFS_QuotaTest is our standard fixture that sets up a
// XFS filesystem on loopback with project quotas enabled.
class ROOT_XFS_QuotaTest : public ROOT_XFS_TestBase
{
public:
  ROOT_XFS_QuotaTest()
    : ROOT_XFS_TestBase("prjquota") {}
};


// ROOT_XFS_NoQuota sets up an XFS filesystem on loopback
// with no quotas enabled.
class ROOT_XFS_NoQuota : public ROOT_XFS_TestBase
{
public:
  ROOT_XFS_NoQuota()
    : ROOT_XFS_TestBase("noquota") {}
};


// ROOT_XFS_NoProjectQuota sets up an XFS filesystem on loopback
// with all the quota types except project quotas enabled.
class ROOT_XFS_NoProjectQuota : public ROOT_XFS_TestBase
{
public:
  ROOT_XFS_NoProjectQuota()
    : ROOT_XFS_TestBase("usrquota,grpquota") {}
};


TEST_F(ROOT_XFS_QuotaTest, QuotaGetSet)
{
  prid_t projectId = 44;
  string root = "project";
  Bytes limit = Megabytes(44);

  ASSERT_SOME(os::mkdir(root));

  EXPECT_SOME(setProjectQuota(root, projectId, limit));

  Result<QuotaInfo> info = getProjectQuota(root, projectId);
  ASSERT_SOME(info);

  EXPECT_EQ(limit, info->hardLimit);
  EXPECT_EQ(Bytes(0), info->used);

  EXPECT_SOME(clearProjectQuota(root, projectId));
}


TEST_F(ROOT_XFS_QuotaTest, QuotaLimit)
{
  prid_t projectId = 55;
  string root = "project";
  Bytes limit = Megabytes(11);
  Bytes used = Megabytes(10);

  ASSERT_SOME(os::mkdir(root));

  // Assign a project quota.
  EXPECT_SOME(setProjectQuota(root, projectId, limit));

  // Move the directory into the project.
  EXPECT_SOME(setProjectId(root, projectId));

  // Allocate some storage to this project.
  EXPECT_SOME(mkfile(path::join(root, "file"), used));

  // And verify the quota reflects what we used.
  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, used),
      getProjectQuota(root, projectId));

  // We have 1MB of our quota left. Verify that we get a write
  // error if we overflow that.
  EXPECT_ERROR(mkfile(path::join(root, "file2"), Megabytes(2)));

  EXPECT_SOME(clearProjectQuota(root, projectId));
}


TEST_F(ROOT_XFS_QuotaTest, ProjectIdErrors)
{
  // Setting project IDs should not work for non-directories.
  EXPECT_SOME(::fs::symlink("symlink", "nowhere"));
  EXPECT_ERROR(setProjectId("symlink", 99));
  EXPECT_ERROR(clearProjectId("symlink"));

  EXPECT_SOME(mkfile("file", Bytes(1)));
  EXPECT_ERROR(setProjectId("file", 99));
  EXPECT_ERROR(clearProjectId("file"));

  // Setting on a missing file should error.
  EXPECT_ERROR(setProjectId("none", 99));
  EXPECT_ERROR(clearProjectId("none"));
}


// Verify that directories are isolated with respect to XFS quotas. We
// create two trees which have symlinks into each other. If we followed
// the symlinks when applying the project IDs to the directories, then the
// quotas would end up being incorrect.
TEST_F(ROOT_XFS_QuotaTest, DirectoryTree)
{
  Bytes limit = Megabytes(100);
  prid_t projectA = 200;
  prid_t projectB = 400;
  string rootA = "projectA";
  string rootB = "projectB";

  // Create rootA with 2MB of data.
  ASSERT_SOME(os::mkdir(path::join(rootA, "depth1/depth2/depth3"), true));
  EXPECT_SOME(mkfile(path::join(rootA, "depth1/file1"), Megabytes(1)));
  EXPECT_SOME(mkfile(path::join(rootA, "depth1/depth2/file2"), Megabytes(1)));

  // Create rootB with 1MB of data.
  ASSERT_SOME(os::mkdir(rootB));
  EXPECT_SOME(mkfile(path::join(rootB, "file1"), Megabytes(1)));

  // Symlink from rootA into rootB. This should have no effect on the
  // measured quota.
  EXPECT_SOME(::fs::symlink(
      path::join(rootB, "file1"), path::join(rootA, "depth1/file1.A")));
  EXPECT_SOME(::fs::symlink(
      path::join(rootB, "file1"), path::join(rootA, "depth1/depth2/file2.A")));
  EXPECT_SOME(::fs::symlink(rootB,
      path::join(rootA, "depth1/depth2/depth3.A")));

  // Now we want to verify that assigning and removing project IDs is recursive
  // and does not follow symlinks. For each directory, assign the project ID and
  // verify the expected quota usage. Then verify the inverse.

  EXPECT_SOME(setProjectId(rootA, projectA));
  EXPECT_SOME(setProjectQuota(rootA, projectA, limit));

  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, Megabytes(2)),
      getProjectQuota(rootA, projectA));

  EXPECT_SOME(setProjectId(rootB, projectB));
  EXPECT_SOME(setProjectQuota(rootB, projectB, limit));

  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, Megabytes(1)),
      getProjectQuota(rootB, projectB));

  EXPECT_SOME(clearProjectId(rootA));

  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, Megabytes(0)),
      getProjectQuota(rootA, projectA));

  EXPECT_SOME(clearProjectId(rootB));

  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, Megabytes(0)),
      getProjectQuota(rootB, projectB));
}


// Verify that a task that tries to consume more space than it has requested
// is only allowed to consume exactly the assigned resources. We tell dd
// to write 2MB but only give it 1MB of resources and (roughly) verify that
// it exits with a failure (that should be a write error).
TEST_F(ROOT_XFS_QuotaTest, DiskUsageExceedsQuota)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), CreateSlaveFlags());
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
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=2");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&failedStatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  AWAIT_READY(failedStatus);
  EXPECT_EQ(task.task_id(), failedStatus->task_id());
  EXPECT_EQ(TASK_FAILED, failedStatus->state());

  // Unlike the 'disk/du' isolator, the reason for task failure
  // should be that dd got an IO error.
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, failedStatus->source());
  EXPECT_EQ("Command exited with status 1", failedStatus->message());

  driver.stop();
  driver.join();
}


// This is the same logic as DiskUsageExceedsQuota except we turn off disk quota
// enforcement, so exceeding the quota should be allowed.
TEST_F(ROOT_XFS_QuotaTest, DiskUsageExceedsQuotaNoEnforce)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.enforce_container_disk_quota = false;

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
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=2");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> finishedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&finishedStatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  // We expect the task to succeed even though it exceeded
  // the disk quota.
  AWAIT_READY(finishedStatus);
  EXPECT_EQ(task.task_id(), finishedStatus->task_id());
  EXPECT_EQ(TASK_FINISHED, finishedStatus->state());

  driver.stop();
  driver.join();
}


// Verify that when the `xfs_kill_containers` flag is enabled, tasks that
// exceed their disk quota are killed with the correct container limitation.
TEST_F(ROOT_XFS_QuotaTest, DiskUsageExceedsQuotaWithKill)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  // Enable killing containers on disk quota violations.
  flags.xfs_kill_containers = true;

  // Tune the watch interval down so that the isolator will detect
  // the quota violation as soon as possible.
  flags.container_disk_watch_interval = Milliseconds(1);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), flags);
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
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses 2MB. This
  // waits a long time to ensure that the task lives long enough for the
  // isolator to impose a container limitation.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=2 && sleep 100000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&killedStatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  AWAIT_READY(killedStatus);
  EXPECT_EQ(task.task_id(), killedStatus->task_id());
  EXPECT_EQ(TASK_FAILED, killedStatus->state());

  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, killedStatus->source());
  EXPECT_EQ(
      TaskStatus::REASON_CONTAINER_LIMITATION_DISK, killedStatus->reason());

  ASSERT_TRUE(killedStatus->has_limitation())
    << JSON::protobuf(killedStatus.get());

  Resources limit = Resources(killedStatus->limitation().resources());

  // Expect that we were limited on a single disk resource that represents
  // the amount of disk that the task consumed. The task used up to 2MB
  // and the the executor logs might use more, but as long we report that
  // the task used more than the 1MB in its resources, we are happy.
  EXPECT_EQ(1u, limit.size());
  ASSERT_SOME(limit.disk());

  // Currently the disk() function performs a static cast to uint64 so
  // fractional Megabytes are truncated.
  EXPECT_GE(limit.disk().get(), Megabytes(1));

  driver.stop();
  driver.join();
}


// Verify that we can get accurate resource statistics from the XFS
// disk isolator.
TEST_F(ROOT_XFS_QuotaTest, ResourceStatistics)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

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

  // Create a task that uses 4 of 3MB disk but doesn't fail. We will verify
  // that the allocated disk is filled.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:3").get(),
      "dd if=/dev/zero of=file bs=1048576 count=4 || sleep 1000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());
  Timeout timeout = Timeout::in(Seconds(5));

  while (true) {
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    EXPECT_EQ(Megabytes(3), Bytes(usage->disk_limit_bytes()));

    if (usage->has_disk_used_bytes()) {
      // Usage must always be <= the limit.
      EXPECT_LE(usage->disk_used_bytes(), usage->disk_limit_bytes());

      // Usage might not be equal to the limit, but it must hit
      // and not exceed the limit.
      if (usage->disk_used_bytes() >= usage->disk_limit_bytes()) {
        EXPECT_EQ(
            usage->disk_used_bytes(), usage->disk_limit_bytes());
        EXPECT_EQ(Megabytes(3), Bytes(usage->disk_used_bytes()));
        break;
      }
    }

    ASSERT_FALSE(timeout.expired());
    os::sleep(Milliseconds(100));
  }

  driver.stop();
  driver.join();
}


// This is the same logic as ResourceStatistics, except the task should
// be allowed to exceed the disk quota, and usage statistics should report
// that the quota was exceeded.
TEST_F(ROOT_XFS_QuotaTest, ResourceStatisticsNoEnforce)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.enforce_container_disk_quota = false;

  Fetcher fetcher(flags);
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

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
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Create a task that uses 4MB of 3MB disk and fails if it can't
  // write the full amount.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:3").get(),
      "dd if=/dev/zero of=file bs=1048576 count=4 && sleep 1000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());
  Duration diskTimeout = Seconds(5);
  Timeout timeout = Timeout::in(diskTimeout);

  while (true) {
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    EXPECT_EQ(Megabytes(3), Bytes(usage->disk_limit_bytes()));

    if (usage->has_disk_used_bytes()) {
      if (usage->disk_used_bytes() >= Megabytes(4).bytes()) {
        break;
      }
    }

    // The stopping condition for this test is that the isolator is
    // able to report that we wrote the full amount of data without
    // being constrained by the task disk limit.
    EXPECT_LE(usage->disk_used_bytes(), Megabytes(4).bytes());

    ASSERT_FALSE(timeout.expired())
      << "Used " << Bytes(usage->disk_used_bytes())
      << " of expected " << Megabytes(4)
      << " within the " << diskTimeout << " timeout";

    os::sleep(Milliseconds(100));
  }

  driver.stop();
  driver.join();
}


// In this test, the framework is not checkpointed. This ensures that when we
// stop the slave, the executor is killed and we will need to recover the
// working directories without getting any checkpointed recovery state.
TEST_F(ROOT_XFS_QuotaTest, NoCheckpointRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

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
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=1; sleep 1000");

  Future<TaskStatus> runningStatus;
  Future<TaskStatus> startingStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(Return());

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  Future<ResourceUsage> usage1 =
    process::dispatch(slave.get()->pid, &Slave::usage);
  AWAIT_READY(usage1);

  // We should have 1 executor using resources.
  ASSERT_EQ(1, usage1->executors().size());

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *containers->begin();

  // Restart the slave.
  slave.get()->terminate();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  containerizer.reset(_containerizer.get());

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);
  AWAIT_READY_FOR(_recover, Seconds(60));

  // Wait until the orphan containers are cleaned up.
  AWAIT_READY_FOR(containerizer.get()->wait(containerId), Seconds(60));
  AWAIT_READY(slaveReregisteredMessage);

  Future<ResourceUsage> usage2 =
    process::dispatch(slave.get()->pid, &Slave::usage);
  AWAIT_READY(usage2);

  // We should have no executors left because we didn't checkpoint.
  ASSERT_TRUE(usage2->executors().empty());

  Try<std::list<string>> sandboxes = getSandboxes();
  ASSERT_SOME(sandboxes);

  // One sandbox and one symlink.
  ASSERT_EQ(2u, sandboxes->size());

  // Scan the remaining sandboxes and make sure that no projects are assigned.
  foreach (const string& sandbox, sandboxes.get()) {
    // Skip the "latest" symlink.
    if (os::stat::islink(sandbox)) {
      continue;
    }

    EXPECT_NONE(xfs::getProjectId(sandbox));
  }

  driver.stop();
  driver.join();
}


// In this test, the framework is checkpointed so we expect the executor to
// persist across the slave restart and to have the same resource usage before
// and after.
TEST_F(ROOT_XFS_QuotaTest, CheckpointRecovery)
{
  slave::Flags flags = CreateSlaveFlags();
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), CreateSlaveFlags());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=1; sleep 1000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  Future<ResourceUsage> usage1 =
    process::dispatch(slave.get()->pid, &Slave::usage);
  AWAIT_READY(usage1);

  // We should have 1 executor using resources.
  ASSERT_EQ(1, usage1->executors().size());

  // Restart the slave.
  slave.get()->terminate();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  Future<ResourceUsage> usage2 =
    process::dispatch(slave.get()->pid, &Slave::usage);
  AWAIT_READY(usage2);

  // We should have still have 1 executor using resources.
  ASSERT_EQ(1, usage1->executors().size());

  Try<std::list<string>> sandboxes = getSandboxes();
  ASSERT_SOME(sandboxes);

  // One sandbox and one symlink.
  ASSERT_EQ(2u, sandboxes->size());

  // Scan the remaining sandboxes. We ought to still have project IDs
  // assigned to them all.
  foreach (const string& sandbox, sandboxes.get()) {
    // Skip the "latest" symlink.
    if (os::stat::islink(sandbox)) {
      continue;
    }

    EXPECT_SOME(xfs::getProjectId(sandbox));
  }

  driver.stop();
  driver.join();
}


// In this test, the agent initially doesn't enable disk isolation
// but then restarts with XFS disk isolation enabled. We verify that
// the old container launched before the agent restart is
// successfully recovered.
TEST_F(ROOT_XFS_QuotaTest, RecoverOldContainers)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  // `CreateSlaveFlags()` enables `disk/xfs` so here we reset
  // `isolation` to empty.
  flags.isolation.clear();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1024 count=1; sleep 1000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningstatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningstatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningstatus);
  EXPECT_EQ(task.task_id(), runningstatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningstatus->state());

  {
    Future<ResourceUsage> usage =
      process::dispatch(slave.get()->pid, &Slave::usage);
    AWAIT_READY(usage);

    // We should have 1 executor using resources but it doesn't have
    // disk limit enabled.
    ASSERT_EQ(1, usage->executors().size());
    const ResourceUsage_Executor& executor = usage->executors().Get(0);
    ASSERT_TRUE(executor.has_statistics());
    ASSERT_FALSE(executor.statistics().has_disk_limit_bytes());
  }

  // Restart the slave.
  slave.get()->terminate();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // This time use the agent flags that include XFS disk isolation.
  slave = StartSlave(detector.get(), CreateSlaveFlags());
  ASSERT_SOME(slave);

  // Wait for the slave to reregister.
  AWAIT_READY(slaveReregisteredMessage);

  {
    Future<ResourceUsage> usage =
      process::dispatch(slave.get()->pid, &Slave::usage);
    AWAIT_READY(usage);

    // We should still have 1 executor using resources but it doesn't
    // have disk limit enabled.
    ASSERT_EQ(1, usage->executors().size());
    const ResourceUsage_Executor& executor = usage->executors().Get(0);
    ASSERT_TRUE(executor.has_statistics());
    ASSERT_FALSE(executor.statistics().has_disk_limit_bytes());
  }

  driver.stop();
  driver.join();
}


TEST_F(ROOT_XFS_QuotaTest, IsolatorFlags)
{
  slave::Flags flags;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // work_dir must be an XFS filesystem.
  flags = CreateSlaveFlags();
  flags.work_dir = "/proc";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // 0 is an invalid project ID.
  flags = CreateSlaveFlags();
  flags.xfs_project_range = "[0-10]";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Project IDs are 32 bit.
  flags = CreateSlaveFlags();
  flags.xfs_project_range = "[100-1099511627776]";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Project IDs must be a range.
  flags = CreateSlaveFlags();
  flags.xfs_project_range = "foo";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Project IDs must be a range.
  flags = CreateSlaveFlags();
  flags.xfs_project_range = "100";
  ASSERT_ERROR(StartSlave(detector.get(), flags));
}


// Verify that we correctly detect when quotas are not enabled at all.
TEST_F(ROOT_XFS_NoQuota, CheckQuotaEnabled)
{
  EXPECT_SOME_EQ(false, xfs::isQuotaEnabled(mountPoint.get()));
  EXPECT_ERROR(XfsDiskIsolatorProcess::create(CreateSlaveFlags()));
}


// Verify that we correctly detect when quotas are enabled but project
// quotas are not enabled.
TEST_F(ROOT_XFS_NoProjectQuota, CheckQuotaEnabled)
{
  EXPECT_SOME_EQ(false, xfs::isQuotaEnabled(mountPoint.get()));
  EXPECT_ERROR(XfsDiskIsolatorProcess::create(CreateSlaveFlags()));
}


// Verify that we correctly detect that project quotas are enabled.
TEST_F(ROOT_XFS_QuotaTest, CheckQuotaEnabled)
{
  EXPECT_SOME_EQ(true, xfs::isQuotaEnabled(mountPoint.get()));
}


TEST(XFS_QuotaTest, BasicBlocks)
{
  // 0 is the same for blocks and bytes.
  EXPECT_EQ(BasicBlocks(0).bytes(), Bytes(0u));

  EXPECT_EQ(BasicBlocks(1).bytes(), Bytes(512));

  // A partial block should round up.
  EXPECT_EQ(Bytes(512), BasicBlocks(Bytes(128)).bytes());
  EXPECT_EQ(Bytes(1024), BasicBlocks(Bytes(513)).bytes());

  EXPECT_EQ(BasicBlocks(1), BasicBlocks(1));
  EXPECT_EQ(BasicBlocks(1), BasicBlocks(Bytes(512)));
}


// TODO(mzhu): Ftype related tests should not be placed in XFS
// quota tests. Move them to a more suitable place. They are
// placed here at the moment due to the XFS dependency (checked by
// `--enable-xfs-disk-isolator`) and we are not ready to introduce
// another configuration flag.

// ROOT_XFS_FtypeOffTest is our standard fixture that sets up
// a XFS filesystem on loopback with ftype option turned on
// (the default setting).
class ROOT_XFS_FtypeOnTest : public ROOT_XFS_TestBase
{
public:
  ROOT_XFS_FtypeOnTest()
    : ROOT_XFS_TestBase(None(), "-n ftype=1 -m crc=1") {}
};

// ROOT_XFS_FtypeOffTest is our standard fixture that sets up a
// XFS filesystem on loopback with ftype option turned off.
class ROOT_XFS_FtypeOffTest : public ROOT_XFS_TestBase
{
public:
  ROOT_XFS_FtypeOffTest()
    : ROOT_XFS_TestBase(None(), "-n ftype=0 -m crc=0") {}
};


// This test verifies that overlayfs backend can be supported
// on the default XFS with ftype option turned on.
TEST_F(ROOT_XFS_FtypeOnTest, OverlayBackendEnabled)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.work_dir = mountPoint.get();
  flags.image_providers = "docker";
  flags.containerizers = "mesos";
  flags.image_provisioner_backend = "overlay";
  flags.docker_registry = path::join(os::getcwd(), "archives");
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);
}


// This test verifies that the overlayfs backend should fail on
// XFS with ftype turned off.
TEST_F(ROOT_XFS_FtypeOffTest, OverlayBackendDisabled)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.work_dir = mountPoint.get();
  flags.image_providers = "docker";
  flags.containerizers = "mesos";
  flags.image_provisioner_backend = "overlay";
  flags.docker_registry = path::join(os::getcwd(), "archives");
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_ERROR(slave);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
