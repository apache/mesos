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

#include "slave/containerizer/mesos/isolators/xfs/disk.hpp"

#include <glog/logging.h>

#include <process/after.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include <stout/os/stat.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/paths.hpp"

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Process;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

static Try<IntervalSet<prid_t>> getIntervalSet(
    const Value::Ranges& ranges)
{
  IntervalSet<prid_t> set;

  for (int i = 0; i < ranges.range_size(); i++) {
    if (ranges.range(i).end() > std::numeric_limits<prid_t>::max()) {
      return Error("Project ID " + stringify(ranges.range(i).end()) +
                   "  is out of range");
    }

    set += (Bound<prid_t>::closed(ranges.range(i).begin()),
            Bound<prid_t>::closed(ranges.range(i).end()));
  }

  return set;
}


static Option<Bytes> getDiskResource(
    const Resources& resources)
{
  Option<Bytes> bytes = None();

  foreach (const Resource& resource, resources) {
    if (resource.name() != "disk") {
      continue;
    }

    // TODO(jpeach): Ignore persistent volume resources. The problem here is
    // that we need to guarantee that we can track the removal of every
    // directory for which we assign a project ID. Since destruction of
    // persistent is not visible to the isolator, we don't want to risk
    // leaking the project ID, or spuriously reusing it.
    if (Resources::isPersistentVolume(resource)) {
      continue;
    }

    if (resource.has_disk() && resource.disk().has_volume()) {
      continue;
    }

    if (bytes.isSome()) {
      bytes.get() += Megabytes(resource.scalar().value());
    } else {
      bytes = Megabytes(resource.scalar().value());
    }
  }

  return bytes;
}


Try<Isolator*> XfsDiskIsolatorProcess::create(const Flags& flags)
{
  if (!xfs::isPathXfs(flags.work_dir)) {
    return Error("'" + flags.work_dir + "' is not an XFS filesystem");
  }

  Try<bool> enabled = xfs::isQuotaEnabled(flags.work_dir);
  if (enabled.isError()) {
    return Error(
        "Failed to get quota status for '" +
        flags.work_dir + "': " + enabled.error());
  }

  if (!enabled.get()) {
    return Error(
        "XFS project quotas are not enabled on '" +
        flags.work_dir + "'");
  }

  Result<uid_t> uid = os::getuid();
  CHECK_SOME(uid) << "getuid(2) doesn't fail";

  if (uid.get() != 0) {
    return Error("The XFS disk isolator requires running as root.");
  }

  Try<Resource> projects =
    Resources::parse("projects", flags.xfs_project_range, "*");

  if (projects.isError()) {
    return Error(
        "Failed to parse XFS project range '" +
        flags.xfs_project_range + "'");
  }

  if (projects->type() != Value::RANGES) {
    return Error(
        "Invalid XFS project resource type " +
        mesos::Value_Type_Name(projects->type()) +
        ", expecting " +
        mesos::Value_Type_Name(Value::RANGES));
  }

  Try<IntervalSet<prid_t>> totalProjectIds =
    getIntervalSet(projects->ranges());

  if (totalProjectIds.isError()) {
    return Error(totalProjectIds.error());
  }

  Option<Error> status = xfs::validateProjectIds(totalProjectIds.get());
  if (status.isSome()) {
    return Error(status->message);
  }

  xfs::QuotaPolicy quotaPolicy = xfs::QuotaPolicy::ACCOUNTING;

  if (flags.enforce_container_disk_quota) {
    quotaPolicy = flags.xfs_kill_containers
      ? xfs::QuotaPolicy::ENFORCING_ACTIVE
      : xfs::QuotaPolicy::ENFORCING_PASSIVE;
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new XfsDiskIsolatorProcess(
          flags.container_disk_watch_interval,
          quotaPolicy, flags.work_dir, totalProjectIds.get())));
}


XfsDiskIsolatorProcess::XfsDiskIsolatorProcess(
    Duration _watchInterval,
    xfs::QuotaPolicy _quotaPolicy,
    const std::string& _workDir,
    const IntervalSet<prid_t>& projectIds)
  : ProcessBase(process::ID::generate("xfs-disk-isolator")),
    watchInterval(_watchInterval),
    quotaPolicy(_quotaPolicy),
    workDir(_workDir),
    totalProjectIds(projectIds),
    freeProjectIds(projectIds)
{
  // At the beginning, the free project range is the same as the
  // configured project range.

  LOG(INFO) << "Allocating XFS project IDs from the range " << totalProjectIds;
}


XfsDiskIsolatorProcess::~XfsDiskIsolatorProcess() {}


Future<Nothing> XfsDiskIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // We don't need to explicitly deal with orphans since we are primarily
  // concerned with the on-disk state. We scan all the sandbox directories
  // for project IDs that we have not recovered and make a best effort to
  // remove all the corresponding on-disk state.
  Try<list<string>> sandboxes = os::glob(path::join(
      paths::getSandboxRootDir(workDir),
      "*",
      "frameworks",
      "*",
      "executors",
      "*",
      "runs",
      "*"));

  if (sandboxes.isError()) {
    return Failure("Failed to scan sandbox directories: " + sandboxes.error());
  }

  hashset<ContainerID> alive;

  foreach (const ContainerState& state, states) {
    alive.insert(state.container_id());
  }

  foreach (const string& sandbox, sandboxes.get()) {
    // Skip the "latest" symlink.
    if (os::stat::islink(sandbox)) {
      continue;
    }

    ContainerID containerId;
    containerId.set_value(Path(sandbox).basename());

    CHECK(!infos.contains(containerId)) << "ContainerIDs should never collide";

    // We fail the isolator recovery upon failure in any container because
    // failing to get the project ID usually suggests some fatal issue on the
    // host.
    Result<prid_t> projectId = xfs::getProjectId(sandbox);
    if (projectId.isError()) {
      return Failure(projectId.error());
    }

    // If there is no project ID, don't worry about it. This can happen the
    // first time an operator enables the XFS disk isolator and we recover a
    // set of containers that we did not isolate.
    if (projectId.isNone()) {
      continue;
    }

    infos.put(containerId, Owned<Info>(new Info(sandbox, projectId.get())));
    freeProjectIds -= projectId.get();

    // If this is a known orphan, the containerizer will send a cleanup call
    // later. If this is a live container, we will manage it. Otherwise, we have
    // to dispatch a cleanup ourselves.  Note that we don't wait for the result
    // of the cleanups as we don't want to block agent recovery for unknown
    // orphans.
    if (!orphans.contains(containerId) && !alive.contains(containerId)) {
      dispatch(self(), &XfsDiskIsolatorProcess::cleanup, containerId);
    }
  }

  return Nothing();
}


// We want to assign the project ID as early as possible. XFS will automatically
// inherit the project ID to new inodes, so if we do this early we save the work
// of manually assigning the ID to a lot of files.
Future<Option<ContainerLaunchInfo>> XfsDiskIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  Option<prid_t> projectId = nextProjectId();
  if (projectId.isNone()) {
    return Failure("Failed to assign project ID, range exhausted");
  }

  // Keep a record of this container so that cleanup() can remove it if
  // we fail to assign the project ID.
  infos.put(
      containerId,
      Owned<Info>(new Info(containerConfig.directory(), projectId.get())));

  Try<Nothing> status = xfs::setProjectId(
      containerConfig.directory(), projectId.get());

  if (status.isError()) {
    return Failure(
        "Failed to assign project " + stringify(projectId.get()) + ": " +
        status.error());
  }

  LOG(INFO) << "Assigned project " << stringify(projectId.get()) << " to '"
            << containerConfig.directory() << "'";

  return update(containerId, containerConfig.resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<ContainerLimitation> XfsDiskIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (infos.contains(containerId)) {
    return infos[containerId]->limitation.future();
  }

  // Any container that did not have a project ID assigned when
  // we recovered it won't be tracked. This will happend when the
  // isolator is first enabled, since we didn't get a chance to
  // assign project IDs to existing containers. We don't want to
  // cause those containers to fail, so we just ignore them.
  LOG(WARNING) << "Ignoring watch for unknown container " << containerId;
  return Future<ContainerLimitation>();
}


Future<Nothing> XfsDiskIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  const Owned<Info>& info = infos[containerId];
  Option<Bytes> needed = getDiskResource(resources);

  if (needed.isNone()) {
    // TODO(jpeach) If there's no disk resource attached, we should set the
    // minimum quota (1 block), since a zero quota would be unconstrained.
    LOG(WARNING) << "Ignoring quota update with no disk resources";
    return Nothing();
  }

  switch (quotaPolicy) {
    case xfs::QuotaPolicy::ACCOUNTING: {
      Try<Nothing> status = xfs::clearProjectQuota(
          info->directory, info->projectId);

      if (status.isError()) {
        return Failure("Failed to clear quota for project " +
                       stringify(info->projectId) + ": " + status.error());
      }

      break;
    }

    case xfs::QuotaPolicy::ENFORCING_ACTIVE:
    case xfs::QuotaPolicy::ENFORCING_PASSIVE: {
      Bytes hardLimit = needed.get();

      // The purpose behind adding to the hard limit is so that the soft
      // limit can be exceeded thereby allowing us to check if the limit
      // has been reached without allowing the process to allocate too
      // much beyond the desired limit.
      if (quotaPolicy == xfs::QuotaPolicy::ENFORCING_ACTIVE) {
        hardLimit += Megabytes(10);
      }

      Try<Nothing> status = xfs::setProjectQuota(
          info->directory, info->projectId, needed.get(), hardLimit);

      if (status.isError()) {
        return Failure("Failed to update quota for project " +
                       stringify(info->projectId) + ": " + status.error());
      }

      LOG(INFO) << "Set quota on container " << containerId
                << " for project " << info->projectId
                << " to " << needed.get() << "/" << hardLimit;

      break;
    }
  }

  info->quota = needed.get();
  return Nothing();
}


void XfsDiskIsolatorProcess::check()
{
  CHECK(quotaPolicy == xfs::QuotaPolicy::ENFORCING_ACTIVE);

  foreachpair(const ContainerID& containerId, const Owned<Info>& info, infos) {
    Result<xfs::QuotaInfo> quotaInfo = xfs::getProjectQuota(
        info->directory, info->projectId);

    if (quotaInfo.isError()) {
      LOG(WARNING) << "Failed to check disk usage for container '"
                   << containerId  << "': " << quotaInfo.error();

      continue;
    }

    // If the soft limit is exceeded the container should be killed.
    if (quotaInfo->used > quotaInfo->softLimit) {
      Resource resource;
      resource.set_name("disk");
      resource.set_type(Value::SCALAR);
      resource.mutable_scalar()->set_value(
        quotaInfo->used.bytes() / Bytes::MEGABYTES);

      info->limitation.set(
          protobuf::slave::createContainerLimitation(
              Resources(resource),
              "Disk usage (" + stringify(quotaInfo->used) +
              ") exceeds quota (" +
              stringify(quotaInfo->softLimit) + ")",
              TaskStatus::REASON_CONTAINER_LIMITATION_DISK));
    }
  }
}


Future<ResourceStatistics> XfsDiskIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring usage for unknown container " << containerId;
    return ResourceStatistics();
  }

  ResourceStatistics statistics;
  const Owned<Info>& info = infos[containerId];

  Result<xfs::QuotaInfo> quota = xfs::getProjectQuota(
      info->directory, info->projectId);

  if (quota.isError()) {
    return Failure(quota.error());
  }

  // If we didn't set the quota (ie. we are in ACCOUNTING mode),
  // the quota limit will be 0. Since we are already tracking
  // what the quota ought to be in the Info, we just always
  // use that.
  statistics.set_disk_limit_bytes(info->quota.bytes());

  if (quota.isSome()) {
    statistics.set_disk_used_bytes(quota->used.bytes());
  }

  return statistics;
}


// Remove all the quota state that was created for this container. We
// make a best effort to remove all the state we can, so we keep going
// even if one operation fails so that we can remove subsequent state.
Future<Nothing> XfsDiskIsolatorProcess::cleanup(const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring cleanup for unknown container " << containerId;
    return Nothing();
  }

  // Take a copy of the Info we are removing so that we can use it
  // to construct the Failure message if necessary.
  const std::string directory = infos[containerId]->directory;
  const prid_t projectId = infos[containerId]->projectId;

  infos.erase(containerId);

  LOG(INFO) << "Removing project ID " << projectId
            << " from '" << directory << "'";

  Try<Nothing> quotaStatus = xfs::clearProjectQuota(
      directory, projectId);

  if (quotaStatus.isError()) {
    LOG(ERROR) << "Failed to clear quota for '"
               << directory << "': " << quotaStatus.error();
  }

  Try<Nothing> projectStatus = xfs::clearProjectId(directory);
  if (projectStatus.isError()) {
    LOG(ERROR) << "Failed to remove project ID "
               << projectId
               << " from '" << directory << "': "
               << projectStatus.error();
  }

  // If we failed to remove the on-disk project ID we can't reclaim it
  // because the quota would then be applied across two containers. This
  // would be a project ID leak, but we could recover it at GC time if
  // that was visible to isolators.
  if (quotaStatus.isError() || projectStatus.isError()) {
    freeProjectIds -= projectId;
    return Failure("Failed to cleanup '" + directory + "'");
  } else {
    returnProjectId(projectId);
    return Nothing();
  }
}


Option<prid_t> XfsDiskIsolatorProcess::nextProjectId()
{
  if (freeProjectIds.empty()) {
    return None();
  }

  prid_t projectId = freeProjectIds.begin()->lower();

  freeProjectIds -= projectId;
  return projectId;
}


void XfsDiskIsolatorProcess::returnProjectId(
    prid_t projectId)
{
  // Only return this project ID to the free range if it is in the total
  // range. This could happen if the total range is changed by the operator
  // and we recover a previous container from the old range.
  if (totalProjectIds.contains(projectId)) {
    freeProjectIds += projectId;
  }
}


void XfsDiskIsolatorProcess::initialize()
{
  process::PID<XfsDiskIsolatorProcess> self(this);

  if (quotaPolicy == xfs::QuotaPolicy::ENFORCING_ACTIVE) {
    // Start a loop to periodically check for containers
    // breaking the soft limit.
    process::loop(
        self,
        [=]() {
          return process::after(watchInterval);
        },
        [=](const Nothing&) -> process::ControlFlow<Nothing> {
          check();
          return process::Continue();
        });
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
