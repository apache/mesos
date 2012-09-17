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

#include <map>
#include <string>
#include <vector>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "logging/logging.hpp"

#include "slave/gc.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {


class GarbageCollectorProcess : public Process<GarbageCollectorProcess>
{
public:
  virtual ~GarbageCollectorProcess();

  // GarbageCollector implementation.
  Future<bool> schedule(const Duration& d, const string& path);

  void prune(const Duration& d);

private:
  void remove(const Timeout& removalTime);

  struct PathInfo
  {
    PathInfo(const string& _path, Promise<bool>* _promise)
      : path(_path), promise(_promise) {}

    string path;
    Promise<bool>* promise;
  };

  // Store all the paths that needed to be deleted after a given timeout.
  // NOTE: We are using std::map here instead of hashmap, because we
  // need the keys of the map (deletion time) to be sorted in ascending order.
  map<Timeout, vector<PathInfo> > paths;

  void reset();
  Timer timer;
};


GarbageCollectorProcess::~GarbageCollectorProcess()
{
  foreachvalue (const vector<PathInfo>& infos, paths) {
    foreach (const PathInfo& info, infos) {
      info.promise->future().discard();
      delete info.promise;
    }
  }
}


Future<bool> GarbageCollectorProcess::schedule(
    const Duration& d,
    const string& path)
{
  LOG(INFO) << "Scheduling " << path << " for removal";

  Promise<bool>* promise = new Promise<bool>();

  Timeout removalTime(d);

  paths[removalTime].push_back(PathInfo(path, promise));

  // If the timer is not yet initialized or the timeout is sooner than
  // the currently active timer, update it.
  if (timer.timeout().remaining() == Seconds(0) ||
      removalTime < timer.timeout()) {
    reset(); // Schedule the timer for next event.
  }

  return promise->future();
}


// Fires a message to self for the next event. This also cancels any
// existing timer.
void GarbageCollectorProcess::reset()
{
  Timer::cancel(timer); // Cancel the existing timer, if any.
  if (!paths.empty()) {
    Timeout removalTime = (*paths.begin()).first; // Get the first entry.
    Duration d = removalTime.remaining();

    VLOG(1) << "Scheduling GC removal event to fire after " << d;
    timer = delay(d, self(), &Self::remove, removalTime);
  } else {
    timer = Timer(); // Reset the timer.
  }
}


void GarbageCollectorProcess::remove(const Timeout& removalTime)
{
  if (paths.count(removalTime) > 0) {
    foreach (const PathInfo& info, paths[removalTime]) {
      const string& path = info.path;
      Promise<bool>* promise = info.promise;

      LOG(INFO) << "Deleting " << path;

      // TODO(benh): Check error conditions of 'rmdir', e.g., permission
      // denied, file no longer exists, etc.
      // TODO(vinod): Consider invoking rmdir via async.
      bool result = os::rmdir(path);

      VLOG(1) << "Deleted " << path;
      promise->set(result);
      delete promise;
    }
    paths.erase(removalTime);
  } else {
    // This might happen if the directory(s) has already been removed
    // (e.g: by prune())
    LOG(WARNING) << "Ignoring gc event at " << removalTime.remaining()
                 << " as the corresponding directories are already removed";
  }

  reset(); // Schedule the timer for next event.
}


void GarbageCollectorProcess::prune(const Duration& d)
{
  foreachkey (const Timeout& removalTime, paths) {
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


Future<bool> GarbageCollector::schedule(
    const Duration& d,
    const string& path)
{
  return dispatch(process, &GarbageCollectorProcess::schedule, d, path);
}


void GarbageCollector::prune(const Duration& d)
{
  return dispatch(process, &GarbageCollectorProcess::prune, d);
}

} // namespace mesos {
} // namespace internal {
} // namespace slave {
