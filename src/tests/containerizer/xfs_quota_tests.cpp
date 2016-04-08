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

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::xfs;

using namespace process;

using std::string;
using std::vector;

using testing::_;
using testing::Return;

using mesos::internal::master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

namespace mesos {
namespace internal {
namespace tests {

static QuotaInfo makeQuotaInfo(
    Bytes limit,
    Bytes used)
{
  return {limit, used};
}


class ROOT_XFS_QuotaTest : public MesosTest
{
public:
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
        Subprocess::PATH("/dev/null"));

    ASSERT_SOME(losetup);
    AWAIT_READY(losetup->status());
    ASSERT_SOME_EQ(0, losetup->status().get());

    loopDevice = loop.get();
    ASSERT_SOME(loopDevice);

    // Make an XFS filesystem (using the force flag). The defaults
    // should be good enough for tests.
    Try<Subprocess> mkfs = subprocess(
        "mkfs.xfs -f " + loopDevice.get(),
        Subprocess::PATH("/dev/null"));

    ASSERT_SOME(mkfs);
    AWAIT_READY(mkfs->status());
    ASSERT_SOME_EQ(0, mkfs->status().get());

    ASSERT_SOME(fs::mount(
        loopDevice.get(),
        mntPath,
        "xfs",
        0, // Flags.
        "prjquota"));
    mountPoint = mntPath;

    ASSERT_SOME(os::chdir(mountPoint.get()))
      << "Failed to chdir into '" << mountPoint.get() << "'";
  }

  virtual void TearDown()
  {
    if (mountPoint.isSome()) {
      fs::unmount(mountPoint.get(), MNT_FORCE | MNT_DETACH);
    }

    // Make a best effort to tear everything down. We don't make any assertions
    // here because even if something goes wrong we still want to clean up as
    // much as we can.
    if (loopDevice.isSome()) {
      Try<Subprocess> cmdProcess = subprocess(
          "losetup -d " + loopDevice.get(),
          Subprocess::PATH("/dev/null"));

      if (cmdProcess.isSome()) {
        cmdProcess->status().await(Seconds(15));
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

  Option<string> loopDevice; // The loop device we attached.
  Option<string> mountPoint; // XFS filesystem mountpoint.
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

  EXPECT_EQ(limit, info.get().limit);
  EXPECT_EQ(Bytes(0), info.get().used);

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
