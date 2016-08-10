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

#ifndef __SLAVE_GC_HPP__
#define __SLAVE_GC_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/multimap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class GarbageCollectorProcess;

// Provides an abstraction for removing files and directories after
// some point at which they are no longer considered necessary to keep
// around. The intent with this abstraction is to also easily enable
// implementations that may actually copy files and directories to
// "more" permanent storage (or provide any other hooks that might be
// useful, e.g., emailing users some time before their files are
// scheduled for removal).
class GarbageCollector
{
public:
  GarbageCollector();
  virtual ~GarbageCollector();

  // Schedules the specified path for removal after the specified
  // duration of time has elapsed. If the path is already scheduled,
  // this will reschedule the removal operation, and induce a discard
  // on the previous future.
  // The future will become ready when the path has been removed.
  // The future will fail if the path did not exist, or on error.
  // The future will be discarded if the path was unscheduled, or
  // was rescheduled.
  // Note that you currently cannot discard a returned future, instead
  // you must call unschedule.
  virtual process::Future<Nothing> schedule(
      const Duration& d,
      const std::string& path);

  // Unschedules the specified path for removal.
  // The future will be true if the path has been unscheduled.
  // The future will be false if the path is not scheduled for
  // removal, or the path has already being removed.
  // Note that you currently cannot discard a returned future.
  virtual process::Future<bool> unschedule(const std::string& path);

  // Deletes all the directories, whose scheduled garbage collection time
  // is within the next 'd' duration of time.
  virtual void prune(const Duration& d);

private:
  GarbageCollectorProcess* process;
};


class GarbageCollectorProcess :
    public process::Process<GarbageCollectorProcess>
{
public:
  GarbageCollectorProcess()
    : ProcessBase(process::ID::generate("agent-garbage-collector")) {}

  virtual ~GarbageCollectorProcess();

  process::Future<Nothing> schedule(
      const Duration& d,
      const std::string& path);

  bool unschedule(const std::string& path);

  void prune(const Duration& d);

private:
  void reset();

  void remove(const process::Timeout& removalTime);

  struct PathInfo
  {
    PathInfo(const std::string& _path,
             process::Owned<process::Promise<Nothing>> _promise)
      : path(_path), promise(_promise) {}

    bool operator==(const PathInfo& that) const
    {
      return path == that.path && promise == that.promise;
    }

    const std::string path;
    const process::Owned<process::Promise<Nothing>> promise;
  };

  // Store all the timeouts and corresponding paths to delete.
  // NOTE: We are using Multimap here instead of Multihashmap, because
  // we need the keys of the map (deletion time) to be sorted.
  Multimap<process::Timeout, PathInfo> paths;

  // We also need efficient lookup for a path, to determine whether
  // it exists in our paths mapping.
  hashmap<std::string, process::Timeout> timeouts;

  process::Timer timer;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_GC_HPP__
