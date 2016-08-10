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

#include <process/delay.hpp>
#include <process/dispatch.hpp>

#include <stout/foreach.hpp>

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
  foreachvalue (const PathInfo& info, paths) {
    info.promise->discard();
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
    CHECK(unschedule(path));
  }

  Owned<Promise<Nothing>> promise(new Promise<Nothing>());

  Timeout removalTime = Timeout::in(d);

  timeouts[path] = removalTime;
  paths.put(removalTime, PathInfo(path, promise));

  // If the timer is not yet initialized or the timeout is sooner than
  // the currently active timer, update it.
  if (timer.timeout().remaining() == Seconds(0) ||
      removalTime < timer.timeout()) {
    reset(); // Schedule the timer for next event.
  }

  return promise->future();
}


bool GarbageCollectorProcess::unschedule(const string& path)
{
  LOG(INFO) << "Unscheduling '" << path << "' from gc";

  if (!timeouts.contains(path)) {
    return false;
  }

  Timeout timeout = timeouts[path]; // Make a copy, as we erase() below.
  CHECK(paths.contains(timeout));

  // Locate the path.
  foreach (const PathInfo& info, paths.get(timeout)) {
    if (info.path == path) {
      // Discard the promise.
      info.promise->discard();

      // Clean up the maps.
      CHECK(paths.remove(timeout, info));
      CHECK(timeouts.erase(path) > 0);

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
  // TODO(bmahler): Other dispatches can block waiting for a removal
  // operation. To fix this, the removal operation can be done
  // asynchronously in another thread.
  if (paths.count(removalTime) > 0) {
    foreach (const PathInfo& info, paths.get(removalTime)) {
      LOG(INFO) << "Deleting " << info.path;

      // Run rmdir with 'continueOnError = true'. It's possible for
      // tasks and isolators to lay down files that are not deletable by
      // GC. In the face of such errors GC needs to free up disk space
      // wherever it can because it's already re-offered to frameworks.
      Try<Nothing> rmdir = os::rmdir(info.path, true, true, true);

      if (rmdir.isError()) {
        LOG(WARNING) << "Failed to delete '" << info.path << "': "
                     << rmdir.error();
        info.promise->fail(rmdir.error());
      } else {
        LOG(INFO) << "Deleted '" << info.path << "'";
        info.promise->set(rmdir.get());
      }

      timeouts.erase(info.path);
    }

    paths.remove(removalTime);
  } else {
    // This occurs when either:
    //   1. The path(s) has already been removed (e.g. by prune()).
    //   2. All paths under the removal time were unscheduled.
    LOG(INFO) << "Ignoring gc event at " << removalTime.remaining()
              << " as the paths were already removed, or were unscheduled";
  }

  reset(); // Schedule the timer for next event.
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
