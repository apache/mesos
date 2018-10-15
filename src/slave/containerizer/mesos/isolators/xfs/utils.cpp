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

// The XFS API headers come from the xfsprogs package. xfsprogs versions
// earlier than 4.5 contain various internal macros that conflict with
// libstdc++.

// If ENABLE_GETTEXT is not defined, then the XFS headers will define
// textdomain() to a while(0) loop. When C++ standard headers try to
// use textdomain(), compilation errors ensue.
#define ENABLE_GETTEXT
#include <xfs/xfs.h>
#undef ENABLE_GETTEXT

// xfs/platform_defs-x86_64.h defines min() and max() macros which conflict
// with various min() and max() function definitions.
#undef min
#undef max

#include <fts.h>

#include <blkid/blkid.h>
#include <linux/dqblk_xfs.h>
#include <linux/quota.h>
#include <sys/quota.h>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/numify.hpp>
#include <stout/path.hpp>

#include <stout/fs.hpp>
#include <stout/os.hpp>

#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

using std::string;

// Manually define this for old kernels. Compatible with the one in
// <linux/quota.h>.
#ifndef PRJQUOTA
#define PRJQUOTA 2
#endif

namespace mesos {
namespace internal {
namespace xfs {

// Although XFS itself doesn't define any invalid project IDs,
// we need a way to know whether or not a project ID was assigned
// so we use 0 as our sentinel value.
static constexpr prid_t NON_PROJECT_ID = 0u;


static Error nonProjectError()
{
  return Error("Invalid project ID '0'");
}


static Try<int> openPath(
    const string& path,
    const struct stat& stat)
{
  int flags = O_NOFOLLOW | O_RDONLY | O_CLOEXEC;

  // Directories require O_DIRECTORY.
  flags |= S_ISDIR(stat.st_mode) ? O_DIRECTORY : 0;
  return os::open(path, flags);
}


static Try<Nothing> setAttributes(
    int fd,
    struct fsxattr& attr)
{
  if (::xfsctl(nullptr, fd, XFS_IOC_FSSETXATTR, &attr) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


static Try<struct fsxattr> getAttributes(int fd)
{
  struct fsxattr attr;

  if (::xfsctl(nullptr, fd, XFS_IOC_FSGETXATTR, &attr) == -1) {
    return ErrnoError();
  }

  return attr;
}


Try<string> getDeviceForPath(const string& path)
{
  struct stat statbuf;

  if (::lstat(path.c_str(), &statbuf) == -1) {
    return ErrnoError("Unable to access '" + path + "'");
  }

  if (S_ISBLK(statbuf.st_mode)) {
    return path;
  }

  char* name = blkid_devno_to_devname(statbuf.st_dev);
  if (name == nullptr) {
    return ErrnoError("Unable to get device for '" + path + "'");
  }

  string devname(name);
  free(name);

  return devname;
}


namespace internal {

static Try<Nothing> setProjectQuota(
    const string& path,
    prid_t projectId,
    Bytes softLimit,
    Bytes hardLimit)
{
  Try<string> devname = getDeviceForPath(path);
  if (devname.isError()) {
    return Error(devname.error());
  }

  fs_disk_quota_t quota = {0};

  quota.d_version = FS_DQUOT_VERSION;

  // Specify that we are setting a project quota for this ID.
  quota.d_id = projectId;
  quota.d_flags = FS_PROJ_QUOTA;
  quota.d_fieldmask = FS_DQ_BSOFT | FS_DQ_BHARD;

  quota.d_blk_hardlimit = BasicBlocks(hardLimit).blocks();
  quota.d_blk_softlimit = BasicBlocks(softLimit).blocks();

  if (::quotactl(QCMD(Q_XSETQLIM, PRJQUOTA),
                 devname->c_str(),
                 projectId,
                 reinterpret_cast<caddr_t>(&quota)) == -1) {
    return ErrnoError("Failed to set quota for project ID " +
                      stringify(projectId));
  }

  return Nothing();
}


static Try<Nothing> setProjectId(
    const string& path,
    const struct stat& stat,
    prid_t projectId)
{
  Try<int> fd = openPath(path, stat);
  if (fd.isError()) {
    return Error("Failed to open '" + path + "': " + fd.error());
  }

  Try<struct fsxattr> attr = getAttributes(fd.get());
  if (attr.isError()) {
    os::close(fd.get());
    return Error("Failed to get XFS attributes for '" + path + "': " +
                 attr.error());
  }

  attr->fsx_projid = projectId;

  if (projectId == NON_PROJECT_ID) {
    attr->fsx_xflags &= ~XFS_XFLAG_PROJINHERIT;
  } else {
    attr->fsx_xflags |= XFS_XFLAG_PROJINHERIT;
  }

  Try<Nothing> status = setAttributes(fd.get(), attr.get());
  os::close(fd.get());

  if (status.isError()) {
    return Error("Failed to set XFS attributes for '" + path + "': " +
                 status.error());
  }

  return Nothing();
}

} // namespace internal {


Result<QuotaInfo> getProjectQuota(
    const string& path,
    prid_t projectId)
{
  if (projectId == NON_PROJECT_ID) {
    return nonProjectError();
  }

  Try<string> devname = getDeviceForPath(path);
  if (devname.isError()) {
    return Error(devname.error());
  }

  fs_disk_quota_t quota = {0};

  quota.d_version = FS_DQUOT_VERSION;
  quota.d_id = projectId;
  quota.d_flags = FS_PROJ_QUOTA;

  // In principle, we should issue a Q_XQUOTASYNC to get an accurate accounting.
  // However, we don't want to affect performance by continually syncing the
  // disks, so we accept that the quota information will be slightly out of
  // date.

  if (::quotactl(QCMD(Q_XGETQUOTA, PRJQUOTA),
                 devname->c_str(),
                 projectId,
                 reinterpret_cast<caddr_t>(&quota)) == -1) {
    return ErrnoError("Failed to get quota for project ID " +
                      stringify(projectId));
  }

  // Zero quota means that no quota is assigned.
  if (quota.d_blk_hardlimit == 0 && quota.d_bcount == 0) {
    return None();
  }

  QuotaInfo info;
  info.softLimit = BasicBlocks(quota.d_blk_softlimit).bytes();
  info.hardLimit = BasicBlocks(quota.d_blk_hardlimit).bytes();
  info.used = BasicBlocks(quota.d_bcount).bytes();

  return info;
}


Try<Nothing> setProjectQuota(
    const string& path,
    prid_t projectId,
    Bytes softLimit,
    Bytes hardLimit)
{
  if (projectId == NON_PROJECT_ID) {
    return nonProjectError();
  }

  // A 0 limit deletes the quota record. If that's desired, the
  // caller should use clearProjectQuota().
  if (hardLimit == 0) {
    return Error("Quota hard limit must be greater than 0");
  }

  if (softLimit == 0) {
    return Error("Quota soft limit must be greater than 0");
  }

  return internal::setProjectQuota(path, projectId, softLimit, hardLimit);
}


Try<Nothing> setProjectQuota(
    const string& path,
    prid_t projectId,
    Bytes hardLimit)
{
  if (projectId == NON_PROJECT_ID) {
    return nonProjectError();
  }

  // A 0 limit deletes the quota record. If that's desired, the
  // caller should use clearProjectQuota().
  if (hardLimit == 0) {
    return Error("Quota limit must be greater than 0");
  }

  return internal::setProjectQuota(path, projectId, hardLimit, hardLimit);
}


Try<Nothing> clearProjectQuota(
    const string& path,
    prid_t projectId)
{
  if (projectId == NON_PROJECT_ID) {
    return nonProjectError();
  }

  return internal::setProjectQuota(path, projectId, Bytes(0), Bytes(0));
}


Result<prid_t> getProjectId(
    const string& directory)
{
  struct stat stat;

  if (::lstat(directory.c_str(), &stat) == -1) {
    return ErrnoError("Failed to access '" + directory);
  }

  Try<int> fd = openPath(directory, stat);
  if (fd.isError()) {
    return Error("Failed to open '" + directory + "': " + fd.error());
  }

  Try<struct fsxattr> attr = getAttributes(fd.get());
  os::close(fd.get());

  if (attr.isError()) {
    return Error("Failed to get XFS attributes for '" + directory + "': " +
                 attr.error());
  }

  if (attr->fsx_projid == NON_PROJECT_ID) {
    return None();
  }

  return attr->fsx_projid;
}


static Try<Nothing> setProjectIdRecursively(
    const string& directory,
    prid_t projectId)
{
  if (os::stat::islink(directory) || !os::stat::isdir(directory)) {
    return Error(directory + " is not a directory");
  }

  char* directory_[] = {const_cast<char*>(directory.c_str()), nullptr};

  FTS* tree = ::fts_open(
      directory_, FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, nullptr);
  if (tree == nullptr) {
    return ErrnoError("Failed to open '" + directory + "'");
  }

  for (FTSENT *node = ::fts_read(tree);
       node != nullptr; node = ::fts_read(tree)) {
    // FTS handles crossing devices (because we use the FTS_XDEV flag), but
    // doesn't know anything about bind mounts made on the same device. Linux
    // doesn't have a direct API for detecting whether a vnode is a mount
    // point, and we prefer to not take the performance cost of looking up
    // each directory in /proc/mounts. We take advantage of the contract
    // that the path lookup checks mount crossings before checking whether
    // the rename is valid. This means that if we attempt an invalid rename
    // operation (i.e. renaming the parent directory to its child), checking
    // for EXDEV tells us whether a mount was crossed.
    //
    // See http://blog.schmorp.de/2016-03-03-detecting-a-mount-point.html
    if (node->fts_info == FTS_D && node->fts_level > 0) {
      CHECK_EQ(-1, ::rename(
          path::join(node->fts_path, "..").c_str(), node->fts_path));

      // If this is a mount point, don't descend any further. Once we skip,
      // FTS will not show us any of the files in this directory.
      if (errno == EXDEV) {
        ::fts_set(tree, node, FTS_SKIP);
        continue;
      }
    }

    if (node->fts_info == FTS_D || node->fts_info == FTS_F) {
      Try<Nothing> status = internal::setProjectId(
          node->fts_path, *node->fts_statp, projectId);
      if (status.isError()) {
        ::fts_close(tree);
        return Error(status.error());
      }
    }
  }

  if (errno != 0) {
    Error error = ErrnoError();
    ::fts_close(tree);
    return error;
  }

  if (::fts_close(tree) != 0) {
    return ErrnoError("Failed to stop traversing file system");
  }

  return Nothing();
}


Try<Nothing> setProjectId(
    const string& directory,
    prid_t projectId)
{
  if (projectId == NON_PROJECT_ID) {
    return nonProjectError();
  }

  return setProjectIdRecursively(directory, projectId);
}


Try<Nothing> clearProjectId(
    const string& directory)
{
  return setProjectIdRecursively(directory, NON_PROJECT_ID);
}


Option<Error> validateProjectIds(const IntervalSet<prid_t>& projectRange)
{
  if (projectRange.contains(NON_PROJECT_ID)) {
    return Error("XFS project ID range contains illegal " +
                 stringify(NON_PROJECT_ID) + " value");
  }

  return None();
}


bool isPathXfs(const string& path)
{
  return ::platform_test_xfs_path(path.c_str()) == 1;
}


Try<bool> isQuotaEnabled(const string& path)
{
  Try<string> devname = getDeviceForPath(path);
  if (devname.isError()) {
    return Error(devname.error());
  }

  struct fs_quota_statv statv = {FS_QSTATV_VERSION1};

  // The quota `type` argument to QCMD() doesn't apply to QCMD_XGETQSTATV
  // since it is for quota subsystem information that can include all
  // types of quotas. Equally, the quotactl() `id` argument doesn't apply
  // because we are getting global information rather than information for
  // a specific identity (eg. a projectId).
  if (::quotactl(QCMD(Q_XGETQSTATV, 0),
                 devname->c_str(),
                 0, // id
                 reinterpret_cast<caddr_t>(&statv)) == -1) {
    // ENOSYS means that quotas are not enabled at all.
    if (errno == ENOSYS) {
      return false;
    }

    return ErrnoError();
  }

  return statv.qs_flags & (FS_QUOTA_PDQ_ACCT | FS_QUOTA_PDQ_ENFD);
}

} // namespace xfs {
} // namespace internal {
} // namespace mesos {
