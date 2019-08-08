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

#include <google/protobuf/util/message_differencer.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/v1/mesos.hpp>

#include <process/gtest.hpp>
#include <process/pid.hpp>

#include <stout/fs.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>

#include "common/values.hpp"

#include "linux/fs.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/gc_process.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/xfs/disk.hpp"
#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

#include "slave/containerizer/mesos/provisioner/backends/overlay.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/docker_archive.hpp"

using namespace mesos::internal::xfs;

using namespace process;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::Return;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::GarbageCollectorProcess;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;
using mesos::internal::slave::XfsDiskIsolatorProcess;

using mesos::internal::slave::paths::getPersistentVolumePath;

using mesos::internal::values::rangesToIntervalSet;

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

  static constexpr int DISK_SIZE_MB = 40;

  virtual void SetUp()
  {
    MesosTest::SetUp();

    Try<string> base = environment->mkdtemp();
    ASSERT_SOME(base) << "Failed to mkdtemp";

    string devPath = path::join(base.get(), "device");
    string mntPath = path::join(base.get(), "mnt");

    ASSERT_SOME(os::mkdir(mntPath));
    ASSERT_SOME(mkfile(devPath, Megabytes(DISK_SIZE_MB)));

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

    const Try<string> mountCmd = mountOptions.isNone()
      ? strings::format("mount -t xfs %s %s", loopDevice.get(), mntPath)
      : strings::format("mount -t xfs -o %s %s %s",
          mountOptions.get(), loopDevice.get(), mntPath);

    Try<Subprocess> mnt = subprocess(
        mountCmd.get(), Subprocess::PATH(os::DEV_NULL));

    ASSERT_SOME(mnt);
    AWAIT_READY(mnt->status());
    ASSERT_SOME_EQ(0, mnt->status().get());

    mountPoint = mntPath;

    ASSERT_SOME(os::chdir(mountPoint.get()))
      << "Failed to chdir into '" << mountPoint.get() << "'";
  }

  virtual void TearDown()
  {
    if (mountPoint.isSome()) {
      Try<Subprocess> umount = subprocess(
          "umount -l -f " + mountPoint.get(), Subprocess::PATH(os::DEV_NULL));

      ASSERT_SOME(umount);
      AWAIT_READY(umount->status());
      ASSERT_SOME_EQ(0, umount->status().get());
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
    flags.isolation = "disk/xfs,filesystem/linux,docker/runtime";
    flags.enforce_container_disk_quota = true;

    // Note that the docker registry doesn't need to be on XFS. The
    // provisioner store does, but we get that via work_dir.
    flags.docker_registry = path::join(sandbox.get(), "registry");
    flags.docker_store_dir = path::join(sandbox.get(), "store");
    flags.image_providers = "docker";

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


constexpr int ROOT_XFS_TestBase::DISK_SIZE_MB;


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


class ROOT_XFS_QuotaEnforcement
  : public ROOT_XFS_TestBase,
    public ::testing::WithParamInterface<ParamDiskQuota::Type>
{
public:
  ROOT_XFS_QuotaEnforcement()
    : ROOT_XFS_TestBase("prjquota") {}
};


INSTANTIATE_TEST_CASE_P(
    Enforcing,
    ROOT_XFS_QuotaEnforcement,
    ::testing::ValuesIn(ParamDiskQuota::parameters()),
    ParamDiskQuota::Printer());


TEST_F(ROOT_XFS_QuotaTest, QuotaGetSet)
{
  prid_t projectId = 44;
  string root = "project";
  Bytes limit = Megabytes(44);

  ASSERT_SOME(os::mkdir(root));

  EXPECT_SOME(setProjectQuota(root, projectId, limit));
  EXPECT_SOME_EQ(
      makeQuotaInfo(limit, 0 /* used */),
      getProjectQuota(root, projectId));

  EXPECT_SOME(clearProjectQuota(root, projectId));

  // After we clear the quota, the quota record is removed and we will get
  // ENOENT trying to read it back.
  EXPECT_ERROR(getProjectQuota(root, projectId));

  EXPECT_SOME_EQ(loopDevice, getDeviceForPath(root));
  EXPECT_SOME_EQ(loopDevice, getDeviceForPath(loopDevice.get()));
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

  // Since this project ID still consumes space, there should still
  // be a quota record. However, the limits should be cleared.
  Result<QuotaInfo> info = getProjectQuota(root, projectId);
  ASSERT_SOME(info);

  EXPECT_EQ(Bytes(0), info->softLimit);
  EXPECT_EQ(Bytes(0), info->hardLimit);

  // We use LE here because we know that the 10MB write succeeded but
  // the 2MB write did not. We could end up using 10MB if the second
  // write atomically failed, or 11MB if it partially succeeded.
  EXPECT_LE(used, info->used);
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
TEST_P(ROOT_XFS_QuotaEnforcement, DiskUsageExceedsQuota)
{
  slave::Flags flags = CreateSlaveFlags();

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

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
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=/tmp/file bs=1048576 count=2");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=file bs=1048576 count=2");
      break;
  }

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


// Verify that a task may not exceed quota on a persistent volume. We run
// a task that writes to a persistent volume and verify that it fails with
// an I/O error. After destroying the volume, make sure that the project
// ID metrics are updated consistently.
TEST_F(ROOT_XFS_QuotaTest, VolumeUsageExceedsQuota)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.gc_delay = Seconds(10);
  flags.disk_watch_interval = Seconds(10);
  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Create a task that requests a 1 MB persistent volume but attempts
  // to use 2MB.
  Resources volume = createPersistentVolume(
      Megabytes(1),
      DEFAULT_TEST_ROLE,
      "id1",
      "volume_path",
      None(),
      None(),
      frameworkInfo.principal());

  // We intentionally request a sandbox that is much bigger (16MB) than
  // the file the task writes (2MB) to the persistent volume (1MB). This
  // makes sure that the quota is indeed enforced on the persistent volume.
  Resources taskResources = Resources::parse(strings::format(
        "cpus:1;mem:32;disk(%s):16", DEFAULT_TEST_ROLE).get()).get() + volume;

  TaskInfo task = createTask(
      offers->at(0).slave_id(),
      taskResources,
      "dd if=/dev/zero of=volume_path/file bs=1048576 count=2 && sleep 1000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> failedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&failedStatus));

  Future<Nothing> terminateExecutor =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<Nothing> gc =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::prune);

  // Create the volume and launch the task.
  driver.acceptOffers(
      {offers->at(0).id()}, {CREATE(volume), LAUNCH({task})});

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

  AWAIT_READY(terminateExecutor);

  // Wait for new offers for the DESTROY.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<ApplyOperationMessage> apply =
    FUTURE_PROTOBUF(ApplyOperationMessage(), _, slave.get()->pid);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Destroy the volume.
  driver.acceptOffers(
      {offers->at(0).id()}, {DESTROY(volume)});

  AWAIT_READY(apply);

  // Advance the clock to trigger sandbox GC.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(gc);

  // Advance the clock to trigger the project ID usage check.
  Clock::pause();
  Clock::advance(flags.disk_watch_interval);
  Clock::settle();
  Clock::resume();

  // We should have reclaimed the project IDs for both the sandbox and the
  // persistent volume.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_total")
        ->as<int>(),
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_free")
        ->as<int>());

  driver.stop();
  driver.join();
}


// Verify that a task may not exceed quota on a persistent volume. We run a
// task that writes to a persistent volume and verify that the containerizer
// kills it when it exceeds the quota. After destroying the volume, make
// sure that the project ID metrics are updated consistently.
TEST_F(ROOT_XFS_QuotaTest, VolumeUsageExceedsQuotaWithKill)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.gc_delay = Seconds(10);
  flags.disk_watch_interval = Seconds(10);
  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  // Enable killing containers on disk quota violations.
  flags.xfs_kill_containers = true;

  // Tune the watch interval down so that the isolator will detect
  // the quota violation as soon as possible.
  flags.container_disk_watch_interval = Milliseconds(1);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // We create 2 persistent volumes, but only write to one so that we can
  // test that the container limitation reports the correct volume in the
  // task resource limitation.
  Resources volume1 = createPersistentVolume(
      Megabytes(1),
      DEFAULT_TEST_ROLE,
      "id1",
      "volume_path_1",
      None(),
      None(),
      frameworkInfo.principal());

  Resources volume2 = createPersistentVolume(
      Megabytes(1),
      DEFAULT_TEST_ROLE,
      "id2",
      "volume_path_2",
      None(),
      None(),
      frameworkInfo.principal());

  // We intentionally request a sandbox that is much bigger (16MB) than
  // the file the task writes (2MB) to the persistent volume (1MB). This
  // makes sure that the quota is indeed enforced on the persistent volume.
  Resources taskResources = Resources::parse(strings::format(
        "cpus:1;mem:32;disk(%s):16", DEFAULT_TEST_ROLE).get()).get();

  TaskInfo task = createTask(
      offers->at(0).slave_id(),
      taskResources + volume1 + volume2,
      "dd if=/dev/zero of=volume_path_1/file bs=1048576 count=2 && "
      "sleep 100000");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&killedStatus));

  Future<Nothing> terminateExecutor =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  Future<Nothing> gc =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::prune);

  // Create the volume and launch the task.
  driver.acceptOffers(
      {offers->at(0).id()},
      {CREATE(volume1), CREATE(volume2), LAUNCH({task})});

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

  // Verify that the `TASK_KILLED` status includes the correct persistent
  // volume disk resource in its limitation.
  ASSERT_TRUE(killedStatus->has_limitation())
    << JSON::protobuf(killedStatus.get());

  ASSERT_EQ(1, killedStatus->limitation().resources().size())
    << JSON::protobuf(killedStatus.get());

  ASSERT_TRUE(killedStatus->limitation().resources().Get(0).has_disk())
    << JSON::protobuf(killedStatus.get());

  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
      killedStatus->limitation().resources().Get(0).disk(),
      Resource(*volume1.begin()).disk()))
    << "Limitation contained disk "
    << killedStatus->limitation().resources().Get(0).disk().DebugString()
    << ", wanted disk " << Resource(*volume1.begin()).disk().DebugString();

  AWAIT_READY(terminateExecutor);

  // Wait for new offers for the DESTROY.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<ApplyOperationMessage> apply =
    FUTURE_PROTOBUF(ApplyOperationMessage(), _, slave.get()->pid);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Destroy the volumes.
  driver.acceptOffers(
      {offers->at(0).id()}, {DESTROY(volume1), DESTROY(volume2)});

  AWAIT_READY(apply);

  // Advance the clock to trigger sandbox GC.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(gc);

  // Advance the clock to trigger the project ID usage check.
  Clock::pause();
  Clock::advance(flags.disk_watch_interval);
  Clock::settle();
  Clock::resume();

  // We should have reclaimed the project IDs for both the sandbox and the
  // persistent volumes.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_total")
        ->as<int>(),
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_free")
        ->as<int>());

  driver.stop();
  driver.join();
}


// This is the same logic as DiskUsageExceedsQuota except we turn off disk quota
// enforcement, so exceeding the quota should be allowed.
TEST_P(ROOT_XFS_QuotaEnforcement, DiskUsageExceedsQuotaNoEnforce)
{
  slave::Flags flags = CreateSlaveFlags();

  flags.enforce_container_disk_quota = false;

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

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
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=/tmp/file bs=1048576 count=2");

      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=file bs=1048576 count=2");
      break;
  }

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
TEST_P(ROOT_XFS_QuotaEnforcement, DiskUsageExceedsQuotaWithKill)
{
  slave::Flags flags = CreateSlaveFlags();

  // Enable killing containers on disk quota violations.
  flags.xfs_kill_containers = true;

  // Tune the watch interval down so that the isolator will detect
  // the quota violation as soon as possible.
  flags.container_disk_watch_interval = Milliseconds(1);

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

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
  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=/tmp/file bs=1048576 count=2");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offer.slave_id(),
          Resources::parse("cpus:1;mem:128;disk:1").get(),
          "pwd; dd if=/dev/zero of=file bs=1048576 count=2");
      break;
  }

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
TEST_P(ROOT_XFS_QuotaEnforcement, ResourceStatistics)
{
  slave::Flags flags = CreateSlaveFlags();

  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

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
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources volume = createPersistentVolume(
        Megabytes(3),
        DEFAULT_TEST_ROLE,
        "id1",
        "path1",
        None(),
        None(),
        frameworkInfo.principal());

  Resources taskResources =
    Resources::parse("cpus:1;mem:128").get() +
    createDiskResource("3", DEFAULT_TEST_ROLE, None(), None()) +
    volume;

  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "echo touch > path1/working && "
          "echo touch > path1/started && "
          "dd if=/dev/zero of=/tmp/file bs=1048576 count=1 && "
          "dd if=/dev/zero of=path1/file bs=1048576 count=1 && "
          "rm path1/working && "
          "sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "echo touch > path1/working && "
          "echo touch > path1/started && "
          "dd if=/dev/zero of=file bs=1048576 count=1 && "
          "dd if=/dev/zero of=path1/file bs=1048576 count=1 && "
          "rm path1/working && "
          "sleep 1000");
      break;
  }

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> killStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&killStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.acceptOffers(
      {offers->at(0).id()},
      {CREATE(volume), LAUNCH({task})});

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
  Timeout timeout = Timeout::in(Seconds(60));

  while (true) {
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_FALSE(timeout.expired());

    // Verify that we can round-trip the ResourceStatistics through a JSON
    // conversion. The v1 operator API depends on this conversion.
    EXPECT_SOME(::protobuf::parse<mesos::v1::ResourceStatistics>(
        JSON::protobuf(usage.get())));

    const string volumePath =
      getPersistentVolumePath(flags.work_dir, DEFAULT_TEST_ROLE, "id1");

    if (!os::exists(path::join(volumePath, "started"))) {
      os::sleep(Milliseconds(100));
      continue;
    }

    if (os::exists(path::join(volumePath, "working"))) {
      os::sleep(Milliseconds(100));
      continue;
    }

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    ASSERT_TRUE(usage->has_disk_used_bytes());

    EXPECT_EQ(Megabytes(3), Bytes(usage->disk_limit_bytes()));
    EXPECT_EQ(Megabytes(1), Bytes(usage->disk_used_bytes()));

    EXPECT_EQ(1, usage->disk_statistics().size());

    foreach (const DiskStatistics& statistics, usage->disk_statistics()) {
      ASSERT_TRUE(statistics.has_limit_bytes());
      ASSERT_TRUE(statistics.has_used_bytes());

      EXPECT_EQ(Megabytes(3), Bytes(statistics.limit_bytes()));

      // We can't precisely control how much ephemeral space is consumed
      // by the rootfs, so check a reasonable range.
      EXPECT_GE(Bytes(statistics.used_bytes()), Megabytes(1));
      EXPECT_LE(Bytes(statistics.used_bytes()), Kilobytes(1400));

      EXPECT_EQ("id1", statistics.persistence().id());
      EXPECT_EQ(
          frameworkInfo.principal(), statistics.persistence().principal());
    }

    break;
  }

  driver.killTask(task.task_id());

  AWAIT_READY(killStatus);
  EXPECT_EQ(task.task_id(), killStatus->task_id());
  EXPECT_EQ(TASK_KILLED, killStatus->state());

  driver.stop();
  driver.join();
}


// This is the same logic as ResourceStatistics, except the task should
// be allowed to exceed the disk quota, and usage statistics should report
// that the quota was exceeded.
TEST_P(ROOT_XFS_QuotaEnforcement, ResourceStatisticsNoEnforce)
{
  slave::Flags flags = CreateSlaveFlags();

  flags.enforce_container_disk_quota = false;

  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

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
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources volume = createPersistentVolume(
        Megabytes(1),
        DEFAULT_TEST_ROLE,
        "id1",
        "path1",
        None(),
        None(),
        frameworkInfo.principal());

  Resources taskResources =
    Resources::parse("cpus:1;mem:128").get() +
    createDiskResource("1", DEFAULT_TEST_ROLE, None(), None()) +
    volume;

  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "echo touch > path1/working && "
          "echo touch > path1/started && "
          "dd if=/dev/zero of=/tmp/file bs=1048576 count=2 && "
          "dd if=/dev/zero of=path1/file bs=1048576 count=2 && "
          "rm path1/working && "
          "sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "echo touch > path1/working && "
          "echo touch > path1/started && "
          "dd if=/dev/zero of=file bs=1048576 count=2 && "
          "dd if=/dev/zero of=path1/file bs=1048576 count=2 && "
          "rm path1/working && "
          "sleep 1000");
      break;
  }

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> killStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&killStatus))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.acceptOffers(
      {offers->at(0).id()},
      {CREATE(volume), LAUNCH({task})});

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
  Duration diskTimeout = Seconds(60);
  Timeout timeout = Timeout::in(diskTimeout);

  while (true) {
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    ASSERT_FALSE(timeout.expired())
      << "Used " << Bytes(usage->disk_used_bytes())
      << " of expected " << Megabytes(2)
      << " within the " << diskTimeout << " timeout";

    const string volumePath =
      getPersistentVolumePath(flags.work_dir, DEFAULT_TEST_ROLE, "id1");

    if (!os::exists(path::join(volumePath, "started"))) {
      os::sleep(Milliseconds(100));
      continue;
    }

    if (os::exists(path::join(volumePath, "working"))) {
      os::sleep(Milliseconds(100));
      continue;
    }

    ASSERT_TRUE(usage->has_disk_limit_bytes());
    ASSERT_TRUE(usage->has_disk_used_bytes());

    EXPECT_EQ(Megabytes(1), Bytes(usage->disk_limit_bytes()));
    EXPECT_EQ(Megabytes(2), Bytes(usage->disk_used_bytes()));

    EXPECT_EQ(1, usage->disk_statistics().size());

    foreach (const DiskStatistics& statistics, usage->disk_statistics()) {
      ASSERT_TRUE(statistics.has_limit_bytes());
      ASSERT_TRUE(statistics.has_used_bytes());

      EXPECT_EQ(Megabytes(1), Bytes(statistics.limit_bytes()));

      // We can't precisely control how much ephemeral space is consumed
      // by the rootfs, so check a reasonable range.
      EXPECT_GE(Bytes(statistics.used_bytes()), Megabytes(2));

      EXPECT_EQ("id1", statistics.persistence().id());
      EXPECT_EQ(
          frameworkInfo.principal(), statistics.persistence().principal());
    }

    break;
  }

  driver.killTask(task.task_id());

  AWAIT_READY(killStatus);
  EXPECT_EQ(task.task_id(), killStatus->task_id());
  EXPECT_EQ(TASK_KILLED, killStatus->state());

  driver.stop();
  driver.join();
}


// In this test, the framework is not checkpointed. This ensures that when we
// stop the slave, the executor is killed and we will need to recover the
// working directories without getting any checkpointed recovery state.
TEST_P(ROOT_XFS_QuotaEnforcement, NoCheckpointRecovery)
{
  slave::Flags flags = CreateSlaveFlags();

  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

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
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources volume = createPersistentVolume(
        Megabytes(1),
        DEFAULT_TEST_ROLE,
        "id1",
        "path1",
        None(),
        None(),
        frameworkInfo.principal());

  Resources taskResources =
    Resources::parse("cpus:1;mem:128").get() +
    createDiskResource("1", DEFAULT_TEST_ROLE, None(), None()) +
    volume;

  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "dd if=/dev/zero of=/tmp/file bs=1048576 count=1; sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "dd if=/dev/zero of=file bs=1048576 count=1; sleep 1000");
      break;
  }

  Future<TaskStatus> runningStatus;
  Future<TaskStatus> startingStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(Return());

  driver.acceptOffers(
      {offers->at(0).id()},
      {CREATE(volume), LAUNCH({task})});

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

  // Restart the slave. We need to delete the containeriner here too,
  // because if we have 2 live containerizers, they will race when adding
  // and removing libprocess metrics.
  slave.get()->terminate();
  slave->reset();
  containerizer.reset();

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

  // Scan the remaining sandboxes and check that project ID is still
  // assigned and that the quota is set.
  foreach (const string& sandbox, sandboxes.get()) {
    // Skip the "latest" symlink.
    if (os::stat::islink(sandbox)) {
      continue;
    }

    Result<prid_t> projectId = xfs::getProjectId(sandbox);
    ASSERT_SOME(projectId);

    EXPECT_SOME(xfs::getProjectQuota(sandbox, projectId.get()));
  }

  {
    const string path =
      getPersistentVolumePath(flags.work_dir, DEFAULT_TEST_ROLE, "id1");

    Result<prid_t> projectId = xfs::getProjectId(path);
    ASSERT_SOME(projectId);

    EXPECT_SOME(xfs::getProjectQuota(path, projectId.get()));
  }

  // Since we are not checkpointing, the rootfs should be gone.
  if (GetParam() == ParamDiskQuota::ROOTFS) {
    int count = 0;

    Try<list<string>> dirs =
      slave::OverlayBackend::listEphemeralVolumes(flags.work_dir);

    ASSERT_SOME(dirs);

    foreach (const string& dir, dirs.get()) {
      Result<prid_t> projectId = xfs::getProjectId(dir);
      ASSERT_FALSE(projectId.isError()) << projectId.error();

      EXPECT_NONE(projectId);
      if (projectId.isSome()) {
        ++count;
      }
    }

    EXPECT_EQ(0, count);
  }

  // We should have project IDs still allocated for the persistent volume and
  // for the task sandbox (since it is not GC'd yet).
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_total")
        ->as<int>() - 2,
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_free")
        ->as<int>());

  driver.stop();
  driver.join();
}


// In this test, the framework is checkpointed so we expect the executor to
// persist across the slave restart and to have the same resource usage before
// and after.
TEST_P(ROOT_XFS_QuotaEnforcement, CheckpointRecovery)
{
  slave::Flags flags = CreateSlaveFlags();

  flags.resources = strings::format(
      "disk(%s):%d", DEFAULT_TEST_ROLE, DISK_SIZE_MB).get();

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);
  frameworkInfo.set_checkpoint(true);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

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

  Resources volume = createPersistentVolume(
        Megabytes(1),
        DEFAULT_TEST_ROLE,
        "id1",
        "path1",
        None(),
        None(),
        frameworkInfo.principal());

  Resources taskResources =
    Resources::parse("cpus:1;mem:128").get() +
    createDiskResource("1", DEFAULT_TEST_ROLE, None(), None()) +
    volume;

  TaskInfo task;

  switch (GetParam()) {
    case ParamDiskQuota::ROOTFS:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "dd if=/dev/zero of=/tmp/file bs=1048576 count=1; sleep 1000");
      task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
      break;
    case ParamDiskQuota::SANDBOX:
      task = createTask(
          offers->at(0).slave_id(),
          taskResources,
          "dd if=/dev/zero of=file bs=1048576 count=1; sleep 1000");
      break;
  }

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.acceptOffers(
      {offers->at(0).id()},
      {CREATE(volume), LAUNCH({task})});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  // We should have assigned a project ID to the persistent volume
  // when the task that uses it started.
  EXPECT_SOME(xfs::getProjectId(
        getPersistentVolumePath(flags.work_dir, DEFAULT_TEST_ROLE, "id1")));

  Future<ResourceUsage> usage1 =
    process::dispatch(slave.get()->pid, &Slave::usage);
  AWAIT_READY(usage1);

  // We should have 1 executor using resources.
  ASSERT_EQ(1, usage1->executors().size());

  // Restart the slave.
  slave.get()->terminate();
  slave->reset();

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

    Result<prid_t> projectId = xfs::getProjectId(sandbox);
    ASSERT_SOME(projectId);

    EXPECT_SOME(xfs::getProjectQuota(sandbox, projectId.get()));
  }

  {
    const string path =
      getPersistentVolumePath(flags.work_dir, DEFAULT_TEST_ROLE, "id1");

    Result<prid_t> projectId = xfs::getProjectId(path);
    ASSERT_SOME(projectId);

    EXPECT_SOME(xfs::getProjectQuota(path, projectId.get()));
  }

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    int count = 0;
    Try<list<string>> dirs =
      slave::OverlayBackend::listEphemeralVolumes(flags.work_dir);

    ASSERT_SOME(dirs);
    EXPECT_FALSE(dirs->empty());

    foreach (const string& dir, dirs.get()) {
      Result<prid_t> projectId = xfs::getProjectId(dir);
      ASSERT_FALSE(projectId.isError()) << projectId.error();

      if (projectId.isSome()) {
        ++count;
        EXPECT_SOME(xfs::getProjectQuota(dir, projectId.get()));
      }
    }

    EXPECT_GT(1, count)
      << "overlay provisioner backend is missing project IDs";
  }

  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_total")
        ->as<int>() - 2,
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_free")
        ->as<int>());

  driver.stop();
  driver.join();
}


// In this test, the agent initially doesn't enable disk isolation
// but then restarts with XFS disk isolation enabled. We verify that
// the old container that was launched before the agent restart is
// successfully recovered.
TEST_P(ROOT_XFS_QuotaEnforcement, RecoverOldContainers)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  // `CreateSlaveFlags()` enables `disk/xfs` so here we reset
  // `isolation` to remove it.
  flags.isolation = "filesystem/linux,docker/runtime";

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

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

  TaskInfo task;

  task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1024 count=1; sleep 1000");

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
  }

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
  slave->reset();

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
    ASSERT_FALSE(executor.statistics().has_disk_used_bytes());
  }

  // Verify that we haven't allocated any project IDs.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_total")
        ->as<int>(),
      metrics.at<JSON::Number>("containerizer/mesos/disk/project_ids_free")
        ->as<int>());

  driver.stop();
  driver.join();
}


// Verify that XFS project IDs are reclaimed when sandbox directories they were
// set on are garbage collected.
TEST_P(ROOT_XFS_QuotaEnforcement, ProjectIdReclaiming)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  flags.gc_delay = Seconds(10);
  flags.disk_watch_interval = Seconds(10);

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    flags.image_provisioner_backend = "overlay";

    AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));
  }

  Try<Resource> projects =
    Resources::parse("projects", flags.xfs_project_range, "*");
  ASSERT_SOME(projects);
  ASSERT_EQ(Value::RANGES, projects->type());
  Try<IntervalSet<prid_t>> totalProjectIds =
    rangesToIntervalSet<prid_t>(projects->ranges());
  ASSERT_SOME(totalProjectIds);

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  JSON::Object metrics = Metrics();
  EXPECT_EQ(totalProjectIds->size(),
            metrics.values["containerizer/mesos/disk/project_ids_total"]);
  EXPECT_EQ(totalProjectIds->size(),
            metrics.values["containerizer/mesos/disk/project_ids_free"]);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Offer offer = offers1->at(0);

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:2").get(),
      "dd if=/dev/zero of=file bs=1048576 count=1 && sleep 1000");

  if (GetParam() == ParamDiskQuota::ROOTFS) {
    task.mutable_container()->CopyFrom(createContainerInfo("test_image"));
  }

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  Future<TaskStatus> exitStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&exitStatus))
    .WillRepeatedly(Return());

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  metrics = Metrics();
  EXPECT_EQ(totalProjectIds->size(),
            metrics.values["containerizer/mesos/disk/project_ids_total"]);
  EXPECT_EQ(totalProjectIds->size() - 1,
            metrics.values["containerizer/mesos/disk/project_ids_free"]);

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  driver.killTask(task.task_id());
  AWAIT_READY(exitStatus);
  EXPECT_EQ(TASK_KILLED, exitStatus->state());

  AWAIT_READY(schedule);

  Try<list<string>> sandboxes = getSandboxes();
  ASSERT_SOME(sandboxes);
  ASSERT_EQ(2u, sandboxes->size());

  // Scan the remaining sandboxes and check that project ID is still
  // assigned and the quota is set.
  Option<prid_t> usedProjectId;

  foreach (const string& sandbox, sandboxes.get()) {
    if (!os::stat::islink(sandbox)) {
      Result<prid_t> projectId = xfs::getProjectId(sandbox);
      ASSERT_SOME(projectId);

      usedProjectId = projectId.get();

      EXPECT_SOME(xfs::getProjectQuota(sandbox, projectId.get()));
    }
  }

  ASSERT_SOME(usedProjectId);

  // Advance the clock to trigger sandbox GC and project ID usage check.
  Clock::pause();
  Clock::advance(flags.gc_delay);
  Clock::settle();
  Clock::advance(flags.disk_watch_interval);
  Clock::settle();
  Clock::resume();

  // Check that the sandbox was GCed.
  sandboxes = getSandboxes();
  ASSERT_SOME(sandboxes);
  ASSERT_TRUE(sandboxes->empty());

  // After the sandbox is removed, we should have reclaimed the project ID,
  // removed all files for the project ID and cleared the quota record.
  // This means that we ought to receive ENOENT when looking up the quota
  // record.
  EXPECT_ERROR(
      xfs::getProjectQuota(mountPoint.get(), usedProjectId.get()));

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  offer = offers2->at(0);

  metrics = Metrics();
  EXPECT_EQ(totalProjectIds->size(),
            metrics.values["containerizer/mesos/disk/project_ids_total"]);
  EXPECT_EQ(totalProjectIds->size(),
            metrics.values["containerizer/mesos/disk/project_ids_free"]);

  task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:2").get(),
      "dd if=/dev/zero of=file bs=1048576 count=1 && sleep 1000");

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillOnce(FutureArg<1>(&exitStatus))
    .WillRepeatedly(Return());

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(task.task_id(), startingStatus->task_id());
  EXPECT_EQ(TASK_STARTING, startingStatus->state());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(task.task_id(), runningStatus->task_id());
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());

  // Scan the sandboxes and check that the project ID was reused.
  sandboxes = getSandboxes();
  ASSERT_SOME(sandboxes);
  EXPECT_EQ(2u, sandboxes->size());
  foreach (const string& sandbox, sandboxes.get()) {
    // Skip the "latest" symlink.
    if (!os::stat::islink(sandbox)) {
      EXPECT_SOME_EQ(usedProjectId.get(), xfs::getProjectId(sandbox));
    }
  }

  metrics = Metrics();
  EXPECT_EQ(totalProjectIds->size() - 1,
            metrics.values["containerizer/mesos/disk/project_ids_free"]);

  driver.killTask(task.task_id());
  AWAIT_READY(exitStatus);
  EXPECT_EQ(TASK_KILLED, exitStatus->state());

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

  // PATH disks that are not on XFS volumes should be rejected.
  flags = CreateSlaveFlags();
  flags.resources = R"(
  [
    {
      "name": "cpus",
      "type": "SCALAR",
      "scalar": {
        "value": 2
      }
    },
    {
      "name": "mem",
      "type": "SCALAR",
      "scalar": {
        "value": 1024
      }
    },
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": {
        "value": 1024
      }
    },
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": { "value": 1024 },
      "disk": {
        "source": {
          "type": "PATH",
          "path": { "root": "/sys" }
        }
      }
    }
  ]
  )";
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
