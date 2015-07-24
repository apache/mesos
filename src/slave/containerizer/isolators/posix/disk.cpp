/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/numify.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/killtree.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/containerizer/isolators/posix/disk.hpp"

using namespace process;

using std::deque;
using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

using mesos::slave::ExecutorLimitation;
using mesos::slave::ExecutorRunState;
using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;

Try<Isolator*> PosixDiskIsolatorProcess::create(const Flags& flags)
{
  // TODO(jieyu): Check the availability of command 'du'.

  return new Isolator(
      process::Owned<IsolatorProcess>(new PosixDiskIsolatorProcess(flags)));
}


PosixDiskIsolatorProcess::Info::PathInfo::~PathInfo()
{
  usage.discard();
}


PosixDiskIsolatorProcess::PosixDiskIsolatorProcess(const Flags& _flags)
  : flags(_flags), collector(flags.container_disk_watch_interval) {}


PosixDiskIsolatorProcess::~PosixDiskIsolatorProcess() {}


Future<Nothing> PosixDiskIsolatorProcess::recover(
    const list<ExecutorRunState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ExecutorRunState& state, states) {
    // Since we checkpoint the executor after we create its working
    // directory, the working directory should definitely exist.
    CHECK(os::exists(state.directory()))
      << "Executor work directory " << state.directory() << " doesn't exist";

    infos.put(state.container_id(), Owned<Info>(new Info(state.directory())));
  }

  return Nothing();
}


Future<Option<CommandInfo>> PosixDiskIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& rootfs,
    const Option<string>& user)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info(directory)));

  return None();
}


Future<Nothing> PosixDiskIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return Nothing();
}


Future<ExecutorLimitation> PosixDiskIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return infos[containerId]->limitation.future();
}


Future<Nothing> PosixDiskIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
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
    if (!resource.has_disk()) {
      // Regular disk used for executor working directory.
      path = info->directory;
    } else {
      // TODO(jieyu): Support persistent volmes as well.
      LOG(ERROR) << "Enforcing disk quota unsupported for " << resource;
      continue;
    }

    quotas[path] += resource;
  }

  // Update the quota for paths. For each new path, we also initiate
  // the disk usage collection.
  foreachpair (const string& path, const Resources& quota, quotas) {
    if (!info->paths.contains(path)) {
      info->paths[path].usage = collector.usage(path)
        .onAny(defer(
            PID<PosixDiskIsolatorProcess>(this),
            &PosixDiskIsolatorProcess::_collect,
            containerId,
            path,
            lambda::_1));
    }

    info->paths[path].quota = quota;
  }

  // Remove paths that we no longer interested in.
  foreach (const string& path, info->paths.keys()) {
    if (!quotas.contains(path)) {
      info->paths.erase(path);
    }
  }

  return Nothing();
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

    if (flags.enforce_container_disk_quota) {
      Option<Bytes> quota = info->paths[path].quota.disk();
      CHECK_SOME(quota);

      if (future.get() > quota.get()) {
        info->limitation.set(protobuf::slave::createExecutorLimitation(
            Resources(info->paths[path].quota),
            "Disk usage (" + stringify(future.get()) +
            ") exceeds quota (" + stringify(quota.get()) + ")"));
      }
    }
  }

  info->paths[path].usage = collector.usage(path)
    .onAny(defer(
        PID<PosixDiskIsolatorProcess>(this),
        &PosixDiskIsolatorProcess::_collect,
        containerId,
        path,
        lambda::_1));
}


Future<ResourceStatistics> PosixDiskIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  ResourceStatistics result;

  const Owned<Info>& info = infos[containerId];

  if (info->paths.contains(info->directory)) {
    Option<Bytes> quota = info->paths[info->directory].quota.disk();
    CHECK_SOME(quota);

    result.set_disk_limit_bytes(quota.get().bytes());

    // NOTE: There may be a large delay (# of containers * interval)
    // until an initial cached value is returned here!
    if (info->paths[info->directory].lastUsage.isSome()) {
      result.set_disk_used_bytes(
          info->paths[info->directory].lastUsage.get().bytes());
    }
  }

  return result;
}


Future<Nothing> PosixDiskIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
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
  DiskUsageCollectorProcess(const Duration& _interval) : interval(_interval) {}
  virtual ~DiskUsageCollectorProcess() {}

  Future<Bytes> usage(const string& path)
  {
    foreach (const Owned<Entry>& entry, entries) {
      if (entry->path == path) {
        return entry->promise.future();
      }
    }

    entries.push_back(Owned<Entry>(new Entry(path)));

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
    explicit Entry(const string& _path) : path(_path) {}

    string path;
    Option<Subprocess> du;
    Promise<Bytes> promise;
  };

  // This function is invoked right before each 'du' is exec'ed. Note
  // that this function needs to be async signal safe.
  static int setupChild()
  {
#ifdef __linux__
    // Kill the child process if the parent exits.
    // NOTE: This function should never returns non-zero because we
    // are passing in a valid signal.
    return ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#else
    return 0;
#endif
  }

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
    Try<Subprocess> s = subprocess(
        "du",
        vector<string>({"du", "-k", "-s", entry->path}),
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        None(),
        None(),
        setupChild);

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

    Future<Option<int>> status = std::get<0>(future.get());

    if (!status.isReady()) {
      entry->promise.fail(
          "Failed to perform 'du': " +
          (status.isFailed() ? status.failure() : "discarded"));
    } else if (status.get().isNone()) {
      entry->promise.fail("Failed to reap the status of 'du'");
    } else if (status.get().get() != 0) {
      Future<string> error = std::get<2>(future.get());
      if (!error.isReady()) {
        entry->promise.fail(
            "Failed to perform 'du'. Reading stderr failed: " +
            (error.isFailed() ? error.failure() : "discarded"));
      } else {
        entry->promise.fail("Failed to perform 'du': " + error.get());
      }
    } else {
      Future<string> output = std::get<1>(future.get());
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


Future<Bytes> DiskUsageCollector::usage(const string& path)
{
  return dispatch(process, &DiskUsageCollectorProcess::usage, path);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
