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

#include <signal.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/types.h>

#include <deque>
#include <tuple>

#include <glog/logging.h>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/strings.hpp>
#include <stout/path.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/stat.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/containerizer/mesos/isolators/posix/disk.hpp"

namespace io = process::io;

using std::deque;
using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Subprocess;

using process::await;
using process::defer;
using process::delay;
using process::dispatch;
using process::spawn;
using process::subprocess;
using process::terminate;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> PosixDiskIsolatorProcess::create(const Flags& flags)
{
  // TODO(jieyu): Check the availability of command 'du'.

  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
        new PosixDiskIsolatorProcess(flags)));
}


PosixDiskIsolatorProcess::Info::PathInfo::~PathInfo()
{
  usage.discard();
}


PosixDiskIsolatorProcess::PosixDiskIsolatorProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("posix-disk-isolator")),
    flags(_flags),
    collector(flags.container_disk_watch_interval) {}


PosixDiskIsolatorProcess::~PosixDiskIsolatorProcess() {}


bool PosixDiskIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Nothing> PosixDiskIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    // If this is a nested container, we do not need to create an Info
    // struct for it because we only perform disk space check for the
    // top level container.
    if (state.container_id().has_parent()) {
      continue;
    }

    // Since we checkpoint the executor after we create its working
    // directory, the working directory should definitely exist.
    CHECK(os::exists(state.directory()))
      << "Executor work directory " << state.directory() << " doesn't exist";

    infos.put(state.container_id(), Owned<Info>(new Info(state.directory())));
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> PosixDiskIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // If this is a nested container, we do not need to create an Info
  // struct for it because we only perform disk space check for the
  // top level container.
  if (containerId.has_parent()) {
    return None();
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info(containerConfig.directory())));

  return None();
}


Future<Nothing> PosixDiskIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (containerId.has_parent()) {
    return Nothing();
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return Nothing();
}


Future<ContainerLimitation> PosixDiskIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // Since we are not doing disk space check for nested containers
  // currently, we simply return a pending future here, indicating
  // that the limit for the nested container will not be reached.
  if (containerId.has_parent()) {
    return Future<ContainerLimitation>();
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return infos[containerId]->limitation.future();
}


Future<Nothing> PosixDiskIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  LOG(INFO) << "Updating the disk resources for container "
            << containerId << " to " << resources;

  const Owned<Info>& info = infos[containerId];

  // This stores the updated quotas.
  hashmap<string, Resources> quotas;

  foreach (const Resource& resource, resources) {
    if (resource.name() != "disk") {
      continue;
    }

    // The path at which we will collect disk usage and enforce quota.
    string path;

    // NOTE: We do not allow the case where has_disk() is true but
    // with nothing set inside DiskInfo. The master will enforce it.
    if (!resource.has_disk() || !resource.disk().has_volume()) {
      // If either DiskInfo or DiskInfo.Volume are not set we're
      // dealing with the working directory of the executor (aka the
      // sandbox).
      path = info->directory;
    } else {
      // Otherwise it is a disk resource (such as a persistent volume)
      // and we extract the path from the protobuf.
      path = resource.disk().volume().container_path();

      // In case the path in the protobuf is not an absolute path it
      // is relative to the working directory of the executor. We
      // always store the absolute path.
      if (!path::absolute(path)) {
        path = path::join(info->directory, path);
      }
    }

    // TODO(jieyu): For persistent volumes, validate that there is
    // only one Resource object associated with it. We could have
    // multiple Resource objects associated with the sandbox because
    // it might be a mix of reserved and unreserved resources.
    quotas[path] += resource;
  }

  // Update the quota for paths. For each new path we also initiate
  // the disk usage collection.
  foreachpair (const string& path, const Resources& quota, quotas) {
    if (!info->paths.contains(path)) {
      info->paths[path].usage = collect(containerId, path);
    }

    info->paths[path].quota = quota;
  }

  // Remove paths that we no longer interested in.
  foreach (const string& path, info->paths.keys()) {
    if (!quotas.contains(path)) {
      // Cancel the usage collection as we are no longer interested.
      info->paths[path].usage.discard();
      info->paths.erase(path);
    }
  }

  return Nothing();
}


Future<Bytes> PosixDiskIsolatorProcess::collect(
    const ContainerID& containerId,
    const string& path)
{
  CHECK(infos.contains(containerId));

  const Owned<Info>& info = infos[containerId];

  // Volume paths to exclude from sandbox disk usage calculation.
  //
  // TODO(jieyu): The 'excludes' list might change when a new
  // persistent volume is added to the list. That might result in the
  // 'du' process to incorrectly include the disk usage of the newly
  // added persistent volume to the usage of the sandbox.
  vector<string> excludes;
  if (path == info->directory) {
    foreachkey (const string& exclude, info->paths) {
      if (exclude != info->directory) {
        excludes.push_back(exclude);
      }
    }
  }

  // We append "/" at the end to make sure that 'du' runs on actual
  // directory pointed by the symlink (and not the symlink itself).
  string _path = path;
  if (path != info->directory && os::stat::islink(path)) {
    _path = path::join(path, "");
  }

  return collector.usage(_path, excludes)
    .onAny(defer(
        PID<PosixDiskIsolatorProcess>(this),
        &PosixDiskIsolatorProcess::_collect,
        containerId,
        path,
        lambda::_1));
}


void PosixDiskIsolatorProcess::_collect(
    const ContainerID& containerId,
    const string& path,
    const Future<Bytes>& future)
{
  if (future.isDiscarded()) {
    LOG(INFO) << "Checking disk usage at '" << path << "' for container "
              << containerId << " has been cancelled";
  } else if (future.isFailed()) {
    LOG(ERROR) << "Checking disk usage at '" << path << "' for container "
               << containerId << " has failed: " << future.failure();
  }

  if (!infos.contains(containerId)) {
    // The container might have just been destroyed.
    return;
  }

  const Owned<Info>& info = infos[containerId];

  if (!info->paths.contains(path)) {
    // The path might have just been removed from this container's
    // resources.
    return;
  }

  // Check if the disk usage exceeds the quota. If yes, report the
  // limitation. We keep collecting the disk usage for 'path' by
  // initiating another round of disk usage check. The check will be
  // throttled by DiskUsageCollector.
  if (future.isReady()) {
    // Save the last disk usage.
    info->paths[path].lastUsage = future.get();

    // We need to ignore the quota enforcement check for MOUNT type
    // disk resources because its quota will be enforced by the
    // underlying filesystem.
    bool isDiskSourceMount = false;
    foreach (const Resource& resource, info->paths[path].quota) {
      if (resource.has_disk() &&
          resource.disk().has_source() &&
          resource.disk().source().type() ==
            Resource::DiskInfo::Source::MOUNT) {
        isDiskSourceMount = true;
      }
    }

    if (flags.enforce_container_disk_quota && !isDiskSourceMount) {
      Option<Bytes> quota = info->paths[path].quota.disk();
      CHECK_SOME(quota);

      if (future.get() > quota.get()) {
        info->limitation.set(
            protobuf::slave::createContainerLimitation(
                Resources(info->paths[path].quota),
                "Disk usage (" + stringify(future.get()) +
                ") exceeds quota (" + stringify(quota.get()) + ")",
                TaskStatus::REASON_CONTAINER_LIMITATION_DISK));
      }
    }
  }

  info->paths[path].usage = collect(containerId, path);
}


Future<ResourceStatistics> PosixDiskIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  ResourceStatistics result;

  const Owned<Info>& info = infos[containerId];

  foreachpair (const string& path,
               const Info::PathInfo& pathInfo,
               info->paths) {
    DiskStatistics *statistics = result.add_disk_statistics();

    Option<Bytes> quota = pathInfo.quota.disk();
    CHECK_SOME(quota);

    statistics->set_limit_bytes(quota->bytes());
    if (path == info->directory) {
      result.set_disk_limit_bytes(quota->bytes());
    }

    // NOTE: There may be a large delay (# of containers * interval)
    // until an initial cached value is returned here!
    if (pathInfo.lastUsage.isSome()) {
      statistics->set_used_bytes(pathInfo.lastUsage->bytes());
      if (path == info->directory) {
        result.set_disk_used_bytes(pathInfo.lastUsage->bytes());
      }
    }

    // Set meta information for persistent volumes.
    if (path != info->directory) {
      // TODO(jieyu): For persistent volumes, validate that there is
      // only one Resource object associated with it.
      Resource resource = *pathInfo.quota.begin();

      if (resource.has_disk() && resource.disk().has_source()) {
        statistics->mutable_source()->CopyFrom(resource.disk().source());
      }

      if (resource.has_disk() && resource.disk().has_persistence()) {
        statistics->mutable_persistence()->CopyFrom(
            resource.disk().persistence());
      }
    }
  }

  return result;
}


Future<Nothing> PosixDiskIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // No need to cleanup anything because we don't create Info struct
  // for nested containers.
  if (containerId.has_parent()) {
    return Nothing();
  }

  if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring cleanup for unknown container " << containerId;
    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


class DiskUsageCollectorProcess : public Process<DiskUsageCollectorProcess>
{
public:
  DiskUsageCollectorProcess(const Duration& _interval)
    : ProcessBase(process::ID::generate("posix-disk-usage-collector")),
      interval(_interval) {}
  virtual ~DiskUsageCollectorProcess() {}

  Future<Bytes> usage(
      const string& path,
      const vector<string>& excludes)
  {
    // TODO(jieyu): 'excludes' is not supported on OSX. We should
    // either return a Failure here, or does not allow 'excludes' to
    // be specified on OSX.

    foreach (const Owned<Entry>& entry, entries) {
      if (entry->path == path) {
        return entry->promise.future();
      }
    }

    entries.push_back(Owned<Entry>(new Entry(path, excludes)));

    // Install onDiscard callback.
    Future<Bytes> future = entries.back()->promise.future();
    future.onDiscard(defer(self(), &Self::discard, path));

    return future;
  }

protected:
  void initialize()
  {
    schedule();
  }

  void finalize()
  {
    foreach (const Owned<Entry>& entry, entries) {
      if (entry->du.isSome() && entry->du.get().status().isPending()) {
        os::killtree(entry->du.get().pid(), SIGKILL);
      }

      entry->promise.fail("DiskUsageCollector is destroyed");
    }
  }

private:
  // Describe a single pending check.
  struct Entry
  {
    explicit Entry(const string& _path, const vector<string>& _excludes)
      : path(_path),
        excludes(_excludes) {}

    string path;
    vector<string> excludes;
    Option<Subprocess> du;
    Promise<Bytes> promise;
  };

  void discard(const string& path)
  {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      // We only cancel those checks whose 'du' haven't been launched.
      if ((*it)->path == path && (*it)->du.isNone()) {
        (*it)->promise.discard();
        entries.erase(it);
        break;
      }
    }
  }

  // Schedule a 'du' to be invoked. The current implementation does
  // not allow multiple 'du's running concurrently. The minimal
  // interval between two subsequent 'du's is controlled by 'interval'
  // for throttling purpose.
  void schedule()
  {
    if (entries.empty()) {
      delay(interval, self(), &Self::schedule);
      return;
    }

    const Owned<Entry>& entry = entries.front();

    // Invoke 'du' and report number of 1K-byte blocks. We fix the
    // block size here so that we can get consistent results on all
    // platforms (e.g., OS X uses 512 byte blocks).
    //
    // NOTE: The 'du' processes are run in the slave's cgroup and it
    // will be that cgroup that is charged for (a) memory to cache the
    // fs data structures, (b) disk I/O to read those structures, and
    // (c) the cpu time to traverse.

    // Construct the 'du' command.
    vector<string> command = {
      "du",
      "-k", // Use 1K size blocks for consistent results across platforms.
      "-s", // Use 'silent' output mode.
    };

#ifdef __linux__
    // Add paths that need to be excluded.
    foreach (const string& exclude, entry->excludes) {
      command.push_back("--exclude");
      command.push_back(exclude);
    }
#endif

    // Add path on which 'du' must be run.
    command.push_back(entry->path);

    // NOTE: The supervisor childhook will watch the parent process and kill
    // the 'du' process in case that the parent die.
    Try<Subprocess> s = subprocess(
        "du",
        command,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        nullptr,
        None(),
        None(),
        {},
        {Subprocess::ChildHook::SUPERVISOR()});

    if (s.isError()) {
      entry->promise.fail("Failed to exec 'du': " + s.error());

      entries.pop_front();
      delay(interval, self(), &Self::schedule);
      return;
    }

    entry->du = s.get();

    await(s.get().status(),
          io::read(s.get().out().get()),
          io::read(s.get().err().get()))
      .onAny(defer(self(), &Self::_schedule, lambda::_1));
  }

  void _schedule(const Future<std::tuple<
      Future<Option<int>>,
      Future<string>,
      Future<string>>>& future)
  {
    CHECK_READY(future);
    CHECK(!entries.empty());

    const Owned<Entry>& entry = entries.front();
    CHECK_SOME(entry->du);

    const Future<Option<int>>& status = std::get<0>(future.get());

    if (!status.isReady()) {
      entry->promise.fail(
          "Failed to perform 'du': " +
          (status.isFailed() ? status.failure() : "discarded"));
    } else if (status.get().isNone()) {
      entry->promise.fail("Failed to reap the status of 'du'");
    } else if (status.get().get() != 0) {
      const Future<string>& error = std::get<2>(future.get());
      if (!error.isReady()) {
        entry->promise.fail(
            "Failed to perform 'du'. Reading stderr failed: " +
            (error.isFailed() ? error.failure() : "discarded"));
      } else {
        entry->promise.fail("Failed to perform 'du': " + error.get());
      }
    } else {
      const Future<string>& output = std::get<1>(future.get());
      if (!output.isReady()) {
        entry->promise.fail(
            "Failed to read stdout from 'du': " +
            (output.isFailed() ? output.failure() : "discarded"));
      } else {
        // Parsing the output from 'du'. The following is a sample
        // output. Tab is used as the delimiter between the number of
        // blocks and the checked path.
        // $ du /var/lib/mesos/.../runs/container_id
        // 1024   /var/lib/mesos/.../runs/container_id
        vector<string> tokens = strings::tokenize(output.get(), " \t");
        if (tokens.empty()) {
          entry->promise.fail("Unexpected output from 'du': " + output.get());
        } else {
          Try<size_t> value = numify<size_t>(tokens[0]);
          if (value.isError()) {
            entry->promise.fail("Unexpected output from 'du': " + output.get());
          } else {
            // Notify the callers.
            entry->promise.set(Kilobytes(value.get()));
          }
        }
      }
    }

    entries.pop_front();
    delay(interval, self(), &Self::schedule);
  }

  const Duration interval;

  // A queue of pending checks.
  deque<Owned<Entry>> entries;
};


DiskUsageCollector::DiskUsageCollector(const Duration& interval)
{
  process = new DiskUsageCollectorProcess(interval);
  spawn(process);
}


DiskUsageCollector::~DiskUsageCollector()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Bytes> DiskUsageCollector::usage(
    const string& path,
    const vector<string>& excludes)
{
  return dispatch(process, &DiskUsageCollectorProcess::usage, path, excludes);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
