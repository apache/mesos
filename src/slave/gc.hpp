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

#ifndef __SLAVE_GC_HPP__
#define __SLAVE_GC_HPP__

#include <map>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/timeout.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
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
  ~GarbageCollector();

  // Schedules the specified path for removal after the specified
  // duration of time has elapsed. The future will become ready when
  // the path has been removed. If the directory did not exist, or an
  // error occurred, the future will fail.
  process::Future<Nothing> schedule(
      const Duration& d,
      const std::string& path);

  // Deletes all the directories, whose scheduled garbage collection
  // time is within the next 'd' duration of time.
  void prune(const Duration& d);

private:
  GarbageCollectorProcess* process;
};


class GarbageCollectorProcess :
    public process::Process<GarbageCollectorProcess>
{
public:
  virtual ~GarbageCollectorProcess();

  // GarbageCollector implementation.
  process::Future<Nothing> schedule(
      const Duration& d,
      const std::string& path);

  void prune(const Duration& d);

private:
  void remove(const process::Timeout& removalTime);

  struct PathInfo
  {
    PathInfo(
        const std::string& _path,
        process::Promise<Nothing>* _promise)
      : path(_path), promise(_promise) {}

    std::string path;
    process::Promise<Nothing>* promise;
  };

  // Store all the paths that needed to be deleted after a given timeout.
  // NOTE: We are using std::map here instead of hashmap, because we
  // need the keys of the map (deletion time) to be sorted in
  // ascending order.
  std::map<process::Timeout, std::vector<PathInfo> > paths;

  void reset();
  process::Timer timer;
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_GC_HPP__
