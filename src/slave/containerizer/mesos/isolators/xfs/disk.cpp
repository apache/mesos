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

#include <process/metrics/metrics.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/unreachable.hpp>
#include <stout/utils.hpp>

#include <stout/os/stat.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/backends/overlay.hpp"

using std::list;
using std::make_pair;
using std::pair;
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


static Try<Nothing> isPathSupported(const string& path)
{
  if (!xfs::isPathXfs(path)) {
    return Error("'" + path + "' is not an XFS filesystem");
  }

  Try<bool> enabled = xfs::isQuotaEnabled(path);
  if (enabled.isError()) {
    return Error(
        "Failed to get quota status for '" + path + "': " + enabled.error());
  }

  if (!enabled.get()) {
    return Error(
        "XFS project quotas are not enabled on '" + path + "'");
  }

  return Nothing();
}


static bool isMountDisk(const Resource::DiskInfo& info)
{
  return info.has_source() &&
    info.source().type() == Resource::DiskInfo::Source::MOUNT;
}


static Option<Bytes> getSandboxDisk(
    const Resources& resources)
{
  Option<Bytes> bytes = None();

  foreach (const Resource& resource, resources) {
    if (resource.name() != "disk") {
      continue;
    }

    if (Resources::isPersistentVolume(resource) || resource.has_disk()) {
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
  Try<Nothing> supported = isPathSupported(flags.work_dir);
  if (supported.isError()) {
    return Error(supported.error());
  }

  // In general, we can't be sure of the filesystem configuration of
  // PATH disks. We verify prior to use so that we don't later have to
  // emit a confusing error (like "inappropriate IOCTL") when attempting
  // to sample the project ID.
  vector<Resource> resources = CHECK_NOTERROR(Resources::fromString(
        flags.resources.getOrElse(""), flags.default_role));

  foreach (const Resource& resource, resources) {
    if (resource.name() != "disk" || !resource.has_disk()) {
      continue;
    }

    const Resource::DiskInfo& diskInfo = resource.disk();

    if (diskInfo.has_source() &&
        diskInfo.source().type() == Resource::DiskInfo::Source::PATH) {
      supported = isPathSupported(diskInfo.source().path().root());
      if (supported.isError()) {
        return Error(supported.error());
      }
    }
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
          quotaPolicy,
          flags.work_dir,
          totalProjectIds.get(),
          flags.disk_watch_interval)));
}


XfsDiskIsolatorProcess::XfsDiskIsolatorProcess(
    Duration _watchInterval,
    xfs::QuotaPolicy _quotaPolicy,
    const std::string& _workDir,
    const IntervalSet<prid_t>& projectIds,
    Duration _projectWatchInterval)
  : ProcessBase(process::ID::generate("xfs-disk-isolator")),
    watchInterval(_watchInterval),
    projectWatchInterval(_projectWatchInterval),
    quotaPolicy(_quotaPolicy),
    workDir(_workDir),
    totalProjectIds(projectIds),
    freeProjectIds(projectIds)
{
  // At the beginning, the free project range is the same as the
  // configured project range.

  LOG(INFO) << "Allocating " << totalProjectIds.size()
            << " XFS project IDs from the range " << totalProjectIds;

  metrics.project_ids_total = totalProjectIds.size();
  metrics.project_ids_free = totalProjectIds.size();
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

    // The operator could have changed the project ID range, so as per
    // returnProjectId(), we should only count this if is is still in range.
    if (totalProjectIds.contains(projectId.get())) {
      --metrics.project_ids_free;
    }

    // If this is a known orphan, the containerizer will send a cleanup call
    // later. If this is a live container, we will manage it. Otherwise, we have
    // to dispatch a cleanup ourselves.  Note that we don't wait for the result
    // of the cleanups as we don't want to block agent recovery for unknown
    // orphans.
    if (!orphans.contains(containerId) && !alive.contains(containerId)) {
      dispatch(self(), &XfsDiskIsolatorProcess::cleanup, containerId);
    }
  }

  foreach (const ContainerState& state, states) {
    foreach (const string& directory, state.ephemeral_volumes()) {
      Result<prid_t> projectId = xfs::getProjectId(directory);
      if (projectId.isError()) {
        return Failure(projectId.error());
      }

      // Ephemeral volumes should have been assigned a project ID, but
      // that's not atommic, so if we were killed during a container
      // launch, we can't guarantee that we labeled all the ephemeral
      // volumes.
      if (projectId.isNone()) {
        if (!infos.contains(state.container_id())) {
          LOG(WARNING) << "Missing project ID for ephemeral volume at '"
                       << directory << "'";
          continue;
        }

        // We have an unlabeled ephemeral volume for a known container. Find
        // the corresponding sandbox path to get the right project ID.
        const Owned<Info>& info = infos.at(state.container_id());

        foreachpair (
            const string& directory,
            const Info::PathInfo& pathInfo,
            info->paths) {
          // Skip persistent volumes.
          if (pathInfo.disk.isSome()) {
            continue;
          }

          Try<Nothing> status =
            xfs::setProjectId(directory, pathInfo.projectId);
          if (status.isError()) {
            return Failure(
                "Failed to assign project " +
                stringify(projectId.get()) + ": " + status.error());
          }

          break;
        }
      }

      Try<Nothing> scheduled = scheduleProjectRoot(projectId.get(), directory);
      if (scheduled.isError()) {
        return Failure(
            "Unable to schedule project ID " + stringify(projectId.get()) +
            " for reclaimation: " + scheduled.error());
      }

      // If we are still managing this project ID, we should have
      // tracked it when we added sandboxes above.
      if (totalProjectIds.contains(projectId.get())) {
          CHECK_NOT_CONTAINS(freeProjectIds, projectId.get());
      }
    }
  }

  Try<list<string>> volumes = paths::getPersistentVolumePaths(workDir);

  if (volumes.isError()) {
    return Failure(
        "Failed to scan persistent volume directories: " + volumes.error());
  }

  // Track any project IDs that we have assigned to persistent volumes. Note
  // that is is possible for operators to delete persistent volumes while
  // the agent isn't running. If that happened, the quota record would be
  // stale, but eventually the project ID would be re-used and the quota
  // updated correctly.
  foreach (const string& directory, volumes.get()) {
    Result<prid_t> projectId = xfs::getProjectId(directory);
    if (projectId.isError()) {
      return Failure(projectId.error());
    }

    if (projectId.isNone()) {
      continue;
    }

    // We should never have duplicate projects IDs assigned
    // to persistent volumes, and since we haven't checked the
    // recovered container resources yet, we can't already know
    // about this persistent volume.
    CHECK(!scheduledProjects.contains(projectId.get()))
        << "Duplicate project ID " << projectId.get()
        << " assigned to '" << directory << "' and '"
        << *scheduledProjects.at(projectId.get()).directories.begin() << "'";

    freeProjectIds -= projectId.get();
    if (totalProjectIds.contains(projectId.get())) {
      --metrics.project_ids_free;
    }

    Try<Nothing> scheduled = scheduleProjectRoot(projectId.get(), directory);
    if (scheduled.isError()) {
      return Failure(
          "Unable to schedule project ID " + stringify(projectId.get()) +
          " for reclaimation: " + scheduled.error());
    }
  }

  // Scan ephemeral provisioner directories to pick up any project IDs that
  // aren't already captured by the recovered container states. Admittedly,
  // it's a bit hacky to specifically check the overlay backend here,
  // but it's not really worse than the sandbox scanning we do above.
  Try<list<string>> provisionerDirs =
    OverlayBackend::listEphemeralVolumes(workDir);

  if (provisionerDirs.isError()) {
    return Failure("Failed to scan overlay provisioner directories: " +
                   provisionerDirs.error());
  }

  foreach (const string& directory, provisionerDirs.get()) {
    if (!os::stat::isdir(directory)) {
      continue;
    }

    Result<prid_t> projectId = xfs::getProjectId(directory);
    if (projectId.isError()) {
      return Failure(projectId.error());
    }

    // Not all provisioner directories will have a project IDs.
    if (projectId.isNone()) {
      continue;
    }

    // It is likely we counted this project ID when we recovered the
    // containers, so don't double-count.
    if (totalProjectIds.contains(projectId.get()) &&
        freeProjectIds.contains(projectId.get())) {
      --metrics.project_ids_free;
      freeProjectIds -= projectId.get();
    }

    Try<Nothing> scheduled = scheduleProjectRoot(projectId.get(), directory);
    if (scheduled.isError()) {
      return Failure(
          "Unable to schedule project ID " + stringify(projectId.get()) +
          " for reclaimation: " + scheduled.error());
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

  LOG(INFO) << "Assigned project " << stringify(projectId.get())
            << " to '" << containerConfig.directory() << "'";

  // The ephemeral volumes share the same quota as the sandbox, so label
  // them with the project ID now.
  foreach (const string& directory, containerConfig.ephemeral_volumes()) {
    Try<Nothing> status = xfs::setProjectId(directory, projectId.get());

    if (status.isError()) {
      return Failure(
          "Failed to assign project " + stringify(projectId.get()) + ": " +
          status.error());
    }

    LOG(INFO) << "Assigned project " << stringify(projectId.get())
              << " to '" << directory << "'";

    Try<Nothing> scheduled = scheduleProjectRoot(projectId.get(), directory);
    if (scheduled.isError()) {
      return Failure(
          "Unable to schedule project ID " + stringify(projectId.get()) +
          " for reclaimation: " + scheduled.error());
    }
  }

  return update(containerId, containerConfig.resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<ContainerLimitation> XfsDiskIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (infos.contains(containerId)) {
    return infos.at(containerId)->limitation.future();
  }

  // Any container that did not have a project ID assigned when
  // we recovered it won't be tracked. This will happend when the
  // isolator is first enabled, since we didn't get a chance to
  // assign project IDs to existing containers. We don't want to
  // cause those containers to fail, so we just ignore them.
  LOG(WARNING) << "Ignoring watch for unknown container " << containerId;
  return Future<ContainerLimitation>();
}


static Try<xfs::QuotaInfo> applyProjectQuota(
    const string& path,
    prid_t projectId,
    Bytes limit,
    xfs::QuotaPolicy quotaPolicy)
{
  switch (quotaPolicy) {
    case xfs::QuotaPolicy::ACCOUNTING: {
      Try<Nothing> status = xfs::clearProjectQuota(path, projectId);

      if (status.isError()) {
        return Error("Failed to clear quota for project " +
                     stringify(projectId) + ": " + status.error());
      }

      return xfs::QuotaInfo();
    }

    case xfs::QuotaPolicy::ENFORCING_ACTIVE:
    case xfs::QuotaPolicy::ENFORCING_PASSIVE: {
      Bytes hardLimit = limit;

      // The purpose behind adding to the hard limit is so that the soft
      // limit can be exceeded thereby allowing us to check if the limit
      // has been reached without allowing the process to allocate too
      // much beyond the desired limit.
      if (quotaPolicy == xfs::QuotaPolicy::ENFORCING_ACTIVE) {
        hardLimit += Megabytes(10);
      }

      Try<Nothing> status = xfs::setProjectQuota(
          path, projectId, limit, hardLimit);

      if (status.isError()) {
        return Error("Failed to update quota for project " +
                     stringify(projectId) + ": " + status.error());
      }

      return xfs::QuotaInfo{limit, hardLimit, 0};
    }
  }

  UNREACHABLE();
}


Future<Nothing> XfsDiskIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  const Owned<Info>& info = infos.at(containerId);

  // First, apply the disk quota to the sandbox.
  Option<Bytes> sandboxQuota = getSandboxDisk(resourceRequests);
  if (sandboxQuota.isSome()) {
    foreachpair (
        const string& directory, Info::PathInfo& pathInfo, info->paths) {
      if (pathInfo.disk.isNone()) {
        pathInfo.quota = sandboxQuota.get();

        Try<xfs::QuotaInfo> status = applyProjectQuota(
            directory, pathInfo.projectId, sandboxQuota.get(), quotaPolicy);
        if (status.isError()) {
          return Failure(status.error());
        }

        LOG(INFO) << "Set quota on container " << containerId
                  << " for project " << pathInfo.projectId
                  << " to " << status->softLimit << "/" << status->hardLimit;
        break;
      }
    }
  }

  // Make sure that we have project IDs assigned to all persistent volumes.
  foreach (const Resource& resource, resourceRequests.persistentVolumes()) {
    CHECK(resource.disk().has_volume());

    const Bytes size = Megabytes(resource.scalar().value());
    const string directory = paths::getPersistentVolumePath(workDir, resource);

    // Don't apply project quotas to mount disks, since they are never
    // subdivided and we can't guarantee that they are XFS filesystems. We
    // still track the path for the container so that we can generate disk
    // statistics correctly.
    if (isMountDisk(resource.disk())) {
      info->paths.put(directory, Info::PathInfo{size, 0, resource.disk()});
      continue;
    }

    Result<prid_t> projectId = xfs::getProjectId(directory);
    if (projectId.isError()) {
      return Failure(projectId.error());
    }

    // If this volume already has a project ID, we must have assigned it
    // here, or assigned or recovered it during project recovery.
    if (projectId.isSome()) {
      CHECK(scheduledProjects.contains(projectId.get()))
        << "untracked project ID " << projectId.get()
        << " for volume ID " << resource.disk().persistence().id()
        << " on " << directory;
    }

    if (projectId.isNone()) {
      Try<Nothing> supported = isPathSupported(directory);
      if (supported.isError()) {
        return Failure(supported.error());
      }

      projectId = nextProjectId();

      Try<Nothing> status = xfs::setProjectId(directory, projectId.get());
      if (status.isError()) {
        return Failure(
            "Failed to assign project " + stringify(projectId.get()) + ": " +
            status.error());
      }

      LOG(INFO) << "Assigned project " << stringify(projectId.get()) << " to '"
                << directory << "'";
    }

    Try<xfs::QuotaInfo> status =
      applyProjectQuota(directory, projectId.get(), size, quotaPolicy);
    if (status.isError()) {
      return Failure(status.error());
    }

    info->paths.put(
        directory, Info::PathInfo{size, projectId.get(), resource.disk()});

    LOG(INFO) << "Set quota on volume " << resource.disk().persistence().id()
              << " for project " << projectId.get()
              << " to " << status->softLimit << "/" << status->hardLimit;

    Try<Nothing> scheduled = scheduleProjectRoot(projectId.get(), directory);
    if (scheduled.isError()) {
      return Failure(
          "Unable to schedule project " + stringify(projectId.get()) +
          " for reclaimation: " + scheduled.error());
    }
  }

  return Nothing();
}


void XfsDiskIsolatorProcess::check()
{
  CHECK(quotaPolicy == xfs::QuotaPolicy::ENFORCING_ACTIVE);

  foreachpair(const ContainerID& containerId, const Owned<Info>& info, infos) {
    foreachpair(
        const string& directory, const Info::PathInfo& pathInfo, info->paths) {
      Result<xfs::QuotaInfo> quotaInfo = xfs::getProjectQuota(
          directory, pathInfo.projectId);

      if (quotaInfo.isError()) {
        LOG(WARNING)
          << "Failed to check disk usage for container '"
          << containerId  << "' in '" << directory << "': "
          << quotaInfo.error();

        continue;
      }

      // If the soft limit is exceeded the container should be killed.
      if (quotaInfo->used > quotaInfo->softLimit) {
        Resource resource;
        resource.set_name("disk");
        resource.set_type(Value::SCALAR);
        resource.mutable_scalar()->set_value(
          quotaInfo->used.bytes() / Bytes::MEGABYTES);

        string volumeInfo;

        if (pathInfo.disk.isSome()) {
          resource.mutable_disk()->CopyFrom(pathInfo.disk.get());
          volumeInfo =
            " for volume '" + pathInfo.disk->persistence().id() + "'";
        }

        LOG(INFO)
          << "Container " << stringify(containerId)
          << " disk usage " << stringify(quotaInfo->used)
          << " exceeded quota " << stringify(quotaInfo->softLimit)
          << volumeInfo
          << " in '" << directory << "'";

        info->limitation.set(
            protobuf::slave::createContainerLimitation(
                Resources(resource),
                "Disk usage (" + stringify(quotaInfo->used) +
                ") exceeded quota (" + stringify(quotaInfo->softLimit) + ")" +
                volumeInfo,
                TaskStatus::REASON_CONTAINER_LIMITATION_DISK));
      }
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

  const Owned<Info>& info = infos.at(containerId);
  ResourceStatistics statistics;

  foreachpair(
      const string& directory, const Info::PathInfo& pathInfo, info->paths) {
    DiskStatistics* disk = nullptr;

    if (pathInfo.disk.isSome()) {
      disk = statistics.add_disk_statistics();
      disk->mutable_persistence()->CopyFrom(pathInfo.disk->persistence());

      // Root disk volumes don't have a source.
      if (pathInfo.disk->has_source()) {
        disk->mutable_source()->CopyFrom(pathInfo.disk->source());
      }
    }

    // We don't require XFS on MOUNT disks, but we still want the isolator
    // to publish usage for them to make them visible to operational tooling.
    if (disk && isMountDisk(pathInfo.disk.get())) {
      Try<Bytes> used = fs::used(directory);
      if (used.isError()) {
        return Failure("Failed to query disk usage for '" + directory +
                      "': " + used.error());
      }

      disk->set_used_bytes(used->bytes());
      continue;
    }

    Result<xfs::QuotaInfo> quotaInfo =
      xfs::getProjectQuota(directory, pathInfo.projectId);
    if (quotaInfo.isError()) {
      return Failure(quotaInfo.error());
    }

    if (quotaInfo.isSome()) {
      if (disk) {
        disk->set_used_bytes(quotaInfo->used.bytes());
      } else {
        statistics.set_disk_used_bytes(quotaInfo->used.bytes());
      }
    }

    // If we didn't set the quota (ie. we are in ACCOUNTING mode),
    // the quota limit will be 0. Since we are already tracking
    // what the quota ought to be in the Info, we just always
    // use that.
    if (disk) {
      disk->set_limit_bytes(pathInfo.quota.bytes());
    } else {
      statistics.set_disk_limit_bytes(pathInfo.quota.bytes());
    }
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

  const Owned<Info>& info = infos.at(containerId);

  // Schedule the directory for project ID reclaimation.
  //
  // We don't reclaim project ID here but wait until sandbox GC time.
  // This is because the sandbox can potentially contain symlinks,
  // from which we can't remove the project ID due to kernel API
  // limitations. Such symlinks would then contribute to disk usage
  // of another container if the project ID was reused causing small
  // inaccuracies in accounting.
  //
  // Fortunately, this behaviour also suffices to reclaim project IDs from
  // persistent volumes. We can just leave the project quota in place until
  // we determine that the persistent volume is no longer present.
  foreachpair (
      const string& directory, const Info::PathInfo& pathInfo, info->paths) {
    Try<Nothing> scheduled = scheduleProjectRoot(pathInfo.projectId, directory);
    if (scheduled.isError()) {
      return Failure(
          "Unable to schedule project " + stringify(pathInfo.projectId) +
          " for reclaimation: " + scheduled.error());
    }
  }

  infos.erase(containerId);
  return Nothing();
}


Option<prid_t> XfsDiskIsolatorProcess::nextProjectId()
{
  if (freeProjectIds.empty()) {
    return None();
  }

  prid_t projectId = freeProjectIds.begin()->lower();

  freeProjectIds -= projectId;
  --metrics.project_ids_free;
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
    ++metrics.project_ids_free;
  }
}


Try<Nothing> XfsDiskIsolatorProcess::scheduleProjectRoot(
    prid_t projectId,
    const string& rootDir)
{
  Try<string> devname = xfs::getDeviceForPath(rootDir);

  if (devname.isError()) {
    return Error(devname.error());
  }

  if (!scheduledProjects.contains(projectId)) {
    scheduledProjects.put(projectId, ProjectRoots{devname.get(), {rootDir}});
  } else {
    ProjectRoots& roots = scheduledProjects.at(projectId);

    if (roots.deviceName != devname.get()) {
      return Error(strings::format(
            "Conflicting device names '%s' and '%s' for project ID %s",
            roots.deviceName, devname.get(), projectId).get());
    }

    roots.directories.insert(rootDir);
  }

  return Nothing();
}

void XfsDiskIsolatorProcess::reclaimProjectIds()
{
  // Note that we need both the directory we assigned the project ID to,
  // and the device node for the block device hosting the directory. Since
  // we can only reclaim the project ID if the former doesn't exist, we
  // need the latter to make the corresponding quota record updates.

  foreachpair (
      prid_t projectId, auto& roots, utils::copy(scheduledProjects)) {
    // Stop tracking any directories that have already been removed.
    foreach (const string& directory, utils::copy(roots.directories)) {
      if (!os::exists(directory)) {
        roots.directories.erase(directory);

        VLOG(1) << "Droppped path '" << directory
                << "' from project ID " << projectId;
      }
    }

    if (roots.directories.empty()) {
      Try<Nothing> status = xfs::clearProjectQuota(roots.deviceName, projectId);
      if (status.isError()) {
        LOG(ERROR) << "Failed to clear quota for project ID "
                   << projectId << "': " << status.error();
      }

      returnProjectId(projectId);
      scheduledProjects.erase(projectId);

      LOG(INFO) << "Reclaimed project ID " << projectId;
    }
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

  // Start a periodic check for which project IDs are currently in use.
  process::loop(
      self,
      [=]() {
        return process::after(projectWatchInterval);
      },
      [=](const Nothing&) -> process::ControlFlow<Nothing> {
        reclaimProjectIds();
        return process::Continue();
      });
}


XfsDiskIsolatorProcess::Metrics::Metrics()
  : project_ids_total("containerizer/mesos/disk/project_ids_total"),
    project_ids_free("containerizer/mesos/disk/project_ids_free")
{
  process::metrics::add(project_ids_total);
  process::metrics::add(project_ids_free);
}


XfsDiskIsolatorProcess::Metrics::~Metrics()
{
  process::metrics::remove(project_ids_free);
  process::metrics::remove(project_ids_total);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
