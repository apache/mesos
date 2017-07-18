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

#include <list>

#include <process/async.hpp>
#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include <stout/os/rmdir.hpp>

#include "logging/logging.hpp"

#include "slave/gc.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::list;
using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {


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

    foreach (const Owned<PathInfo> info, paths.get(removalTime)) {
      if (info->removing) {
        VLOG(1) << "Skipping deletion of '" << info-> path
                << "'  as it is already in progress";
        continue;
      }

      infos.push_back(info);

      // Set `removing` to signify that the path is being cleaned up.
      info->removing = true;
    }

    auto rmdirs = [infos]() {
      foreach (const Owned<PathInfo>& info, infos) {
        // Run the removal operation with 'continueOnError = true'.
        // It's possible for tasks and isolators to lay down files
        // that are not deletable by GC. In the face of such errors
        // GC needs to free up disk space wherever it can because the
        // disk space has already been re-offered to frameworks.
        LOG(INFO) << "Deleting " << info->path;
        Try<Nothing> rmdir = os::rmdir(info->path, true, true, true);

        if (rmdir.isError()) {
          LOG(WARNING) << "Failed to delete '" << info->path << "': "
                       << rmdir.error();
          info->promise.fail(rmdir.error());
        } else {
          LOG(INFO) << "Deleted '" << info->path << "'";
          info->promise.set(rmdir.get());
        }
      }

      return Nothing();
    };

    async(rmdirs)
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


void  GarbageCollectorProcess::_remove(const Future<Nothing>& result,
                                       const list<Owned<PathInfo>> infos)
{
  CHECK_READY(result);

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


GarbageCollector::GarbageCollector()
{
  process = new GarbageCollectorProcess();
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
