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

#ifndef __SLAVE_GC_PROCESS_HPP__
#define __SLAVE_GC_PROCESS_HPP__

#include <list>
#include <string>

#include <process/executor.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/multimap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

class GarbageCollectorProcess :
    public process::Process<GarbageCollectorProcess>
{
public:
  explicit GarbageCollectorProcess(const std::string& _workDir)
    : ProcessBase(process::ID::generate("agent-garbage-collector")),
      metrics(this),
      workDir(_workDir) {}

  ~GarbageCollectorProcess() override;

  process::Future<Nothing> schedule(
      const Duration& d,
      const std::string& path);

  process::Future<bool> unschedule(const std::string& path);

  void prune(const Duration& d);

private:
  void reset();

  void remove(const process::Timeout& removalTime);

  struct PathInfo
  {
    PathInfo(const std::string& _path)
      : path(_path) {}

    bool operator==(const PathInfo& that) const
    {
      return path == that.path;
    }

    const std::string path;

    // This promise tracks the scheduled gc for the path.
    process::Promise<Nothing> promise;

    bool removing = false;
  };

  // Callback for `remove` for bookkeeping after path removal.
  void _remove(
      const process::Future<Nothing>& result,
      const std::list<process::Owned<PathInfo>> infos);

  struct Metrics
  {
    explicit Metrics(GarbageCollectorProcess *gc);
    ~Metrics();

    process::metrics::Counter path_removals_succeeded;
    process::metrics::Counter path_removals_failed;
    process::metrics::PullGauge path_removals_pending;
  } metrics;

  const std::string workDir;

  // Store all the timeouts and corresponding paths to delete.
  // NOTE: We are using Multimap here instead of Multihashmap, because
  // we need the keys of the map (deletion time) to be sorted.
  Multimap<process::Timeout, process::Owned<PathInfo>> paths;

  // We also need efficient lookup for a path, to determine whether
  // it exists in our paths mapping.
  hashmap<std::string, process::Timeout> timeouts;

  process::Timer timer;

  // For executing path removals in a separate actor.
  process::Executor executor;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_GC_PROCESS_HPP__
