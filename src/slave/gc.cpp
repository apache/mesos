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

#include "slave/gc.hpp"

#include <list>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/adaptor.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include <stout/os/rmdir.hpp>

#include "logging/logging.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "slave/gc_process.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::list;
using std::map;
using std::string;

using process::metrics::Counter;

namespace mesos {
namespace internal {
namespace slave {

GarbageCollectorProcess::Metrics::Metrics(GarbageCollectorProcess *gc)
  : path_removals_succeeded("gc/path_removals_succeeded"),
    path_removals_failed("gc/path_removals_failed"),
    path_removals_pending("gc/path_removals_pending", [gc]() {
      // Multimap size is defined to take constant time, which means it
      // basically has to be tracked as a member variable, which means we
      // can safely do concurrent reads while the map is being updated.
      return static_cast<double>(gc->paths.size());
    })
{
  process::metrics::add(path_removals_succeeded);
  process::metrics::add(path_removals_failed);
  process::metrics::add(path_removals_pending);
}


GarbageCollectorProcess::Metrics::~Metrics()
{
  process::metrics::remove(path_removals_succeeded);
  process::metrics::remove(path_removals_failed);

  // Wait for the metric to be removed to protect against asynchronous
  // evaluation referencing a deleted object.
  process::metrics::remove(path_removals_pending).await();
}


GarbageCollectorProcess::~GarbageCollectorProcess()
{
  foreachvalue (const Owned<PathInfo>& info, paths) {
    info->promise.discard();
  }
}


Future<Nothing> GarbageCollectorProcess::schedule(
    const Duration& d,
    const string& path)
{
  LOG(INFO) << "Scheduling '" << path << "' for gc " << d << " in the future";

  // If there's an existing schedule for this path, we must remove
  // it here in order to reschedule.
  if (timeouts.contains(path)) {
    return unschedule(path)
      .then(defer(
          self(),
          &Self::schedule,
          d,
          path));
  }

  Timeout removalTime = Timeout::in(d);

  timeouts[path] = removalTime;

  Owned<PathInfo> info(new PathInfo(path));

  paths.put(removalTime, info);

  // If the timer is not yet initialized or the timeout is sooner than
  // the currently active timer, update it.
  if (timer.timeout().remaining() == Seconds(0) ||
      removalTime < timer.timeout()) {
    reset(); // Schedule the timer for next event.
  }

  return info->promise.future();
}


Future<bool> GarbageCollectorProcess::unschedule(const string& path)
{
  LOG(INFO) << "Unscheduling '" << path << "' from gc";

  if (!timeouts.contains(path)) {
    return false;
  }

  Timeout timeout = timeouts[path]; // Make a copy, as we erase() below.
  CHECK(paths.contains(timeout));

  // Locate the path.
  foreach (const Owned<PathInfo>& info, paths.get(timeout)) {
    if (info->path == path) {
      // If the path is currently undergoing removal, we cannot
      // prevent path removal and wait for removal completion.
      if (info->removing) {
        // Return false to be consistent with the behavior when
        // `unschedule` is called after the path is removed.
        return info->promise.future()
          .then([]() { return false; });
      }

      // Discard the promise.
      info->promise.discard();

      // Clean up the maps.
      CHECK(paths.remove(timeout, info));
      CHECK_EQ(timeouts.erase(info->path), 1u);

      return true;
    }
  }

  LOG(FATAL) << "Inconsistent state across 'paths' and 'timeouts'";
  return false;
}


// Fires a message to self for the next event. This also cancels any
// existing timer.
void GarbageCollectorProcess::reset()
{
  Clock::cancel(timer); // Cancel the existing timer, if any.
  if (!paths.empty()) {
    Timeout removalTime = (*paths.begin()).first; // Get the first entry.

    timer = delay(removalTime.remaining(), self(), &Self::remove, removalTime);
  } else {
    timer = Timer(); // Reset the timer.
  }
}


void GarbageCollectorProcess::remove(const Timeout& removalTime)
{
  if (paths.count(removalTime) > 0) {
    list<Owned<PathInfo>> infos;

    foreach (const Owned<PathInfo>& info, paths.get(removalTime)) {
      if (info->removing) {
        VLOG(1) << "Skipping deletion of '" << info-> path
                << "'  as it is already in progress";
        continue;
      }

      infos.push_back(info);

      // Set `removing` to signify that the path is being cleaned up.
      info->removing = true;
    }

    Counter _succeeded = metrics.path_removals_succeeded;
    Counter _failed = metrics.path_removals_failed;
    const string _workDir = workDir;

    auto rmdirs =
      [_succeeded, _failed, _workDir, infos]() mutable -> Future<Nothing> {
      // Make mutable copies of the counters to work around MESOS-7907.
      Counter succeeded = _succeeded;
      Counter failed = _failed;

#ifdef __linux__
      // Clear any possible persistent volume mount points in `infos`. See
      // MESOS-8830.
      Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
      if (mountTable.isError()) {
        LOG(ERROR) << "Skipping any path deletion because of failure on read "
                      "MountInfoTable for agent process: "
                   << mountTable.error();

        foreach (const Owned<PathInfo>& info, infos) {
          info->promise.fail(mountTable.error());
          ++failed;
        }

        return Failure(mountTable.error());
      }

      foreach (const fs::MountInfoTable::Entry& entry,
               adaptor::reverse(mountTable->entries)) {
        // Ignore mounts whose targets are not under `workDir`.
        if (!strings::startsWith(
                path::join(entry.target, ""),
                path::join(_workDir, ""))) {
                continue;
        }

        for (auto it = infos.begin(); it != infos.end(); ) {
          const Owned<PathInfo>& info = *it;
          // TODO(zhitao): Validate that both `info->path` and `workDir` are
          // real paths.
          if (strings::startsWith(
                path::join(entry.target, ""), path::join(info->path, ""))) {
            LOG(WARNING)
                << "Unmounting dangling mount point '" << entry.target
                << "' of persistent volume '" << entry.root
                << "' inside garbage collected path '" << info->path << "'";

            Try<Nothing> unmount = fs::unmount(entry.target);
            if (unmount.isError()) {
              LOG(WARNING) << "Skipping deletion of '"
                           << info->path << "' because unmount failed on '"
                           << entry.target << "': " << unmount.error();

              info->promise.fail(unmount.error());
              ++failed;
              it = infos.erase(it);
              continue;
            } else {
              break;
            }
          }

          it++;
        }
      }
#endif // __linux__

      foreach (const Owned<PathInfo>& info, infos) {
        // Run the removal operation with 'continueOnError = true'.
        // It's possible for tasks and isolators to lay down files
        // that are not deletable by GC. In the face of such errors
        // GC needs to free up disk space wherever it can because the
        // disk space has already been re-offered to frameworks.
        LOG(INFO) << "Deleting " << info->path;
        Try<Nothing> rmdir = os::rmdir(info->path, true, true, true);

        if (rmdir.isError()) {
          // TODO(zhitao): Change return value type of `rmdir` to
          // `Try<Nothing, ErrnoError>` and check error type instead.
          if (rmdir.error() == ErrnoError(ENOENT).message) {
            LOG(INFO) << "Skipped '" << info->path << "' which does not exist";
          } else {
            LOG(WARNING) << "Failed to delete '" << info->path << "': "
                         << rmdir.error();
            info->promise.fail(rmdir.error());

            ++failed;
          }
        } else {
          LOG(INFO) << "Deleted '" << info->path << "'";
          info->promise.set(rmdir.get());

          ++succeeded;
        }
      }

      return Nothing();
    };

    // NOTE: All `rmdirs` calls are dispatched to one executor so that:
    //   1. They do not block other dispatches (MESOS-6549).
    //   2. They do not occupy all worker threads (MESOS-7964).
    executor.execute(rmdirs)
      .onAny(defer(self(), &Self::_remove, lambda::_1, infos));
  } else {
    // This occurs when either:
    //   1. The path(s) has already been removed (e.g. by prune()).
    //   2. All paths under the removal time were unscheduled.
    LOG(INFO) << "Ignoring gc event at " << removalTime.remaining()
              << " as the paths were already removed, or were unscheduled";
    reset();
  }
}


void GarbageCollectorProcess::_remove(const Future<Nothing>& result,
                                      const list<Owned<PathInfo>> infos)
{
  // Remove path records from `paths` and `timeouts` data structures.
  foreach (const Owned<PathInfo>& info, infos) {
    CHECK(paths.remove(timeouts[info->path], info));
    CHECK_EQ(timeouts.erase(info->path), 1u);
  }

  reset();
}


void GarbageCollectorProcess::prune(const Duration& d)
{
  foreach (const Timeout& removalTime, paths.keys()) {
    if (removalTime.remaining() <= d) {
      LOG(INFO) << "Pruning directories with remaining removal time "
                << removalTime.remaining();
      dispatch(self(), &GarbageCollectorProcess::remove, removalTime);
    }
  }
}


GarbageCollector::GarbageCollector(const string& workDir)
{
  process = new GarbageCollectorProcess(workDir);
  spawn(process);
}


GarbageCollector::~GarbageCollector()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> GarbageCollector::schedule(
    const Duration& d,
    const string& path)
{
  return dispatch(process, &GarbageCollectorProcess::schedule, d, path);
}


Future<bool> GarbageCollector::unschedule(const string& path)
{
  return dispatch(process, &GarbageCollectorProcess::unschedule, path);
}


void GarbageCollector::prune(const Duration& d)
{
  dispatch(process, &GarbageCollectorProcess::prune, d);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
