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

#include <string>

#include <process/future.hpp>

#include <stout/duration.hpp>

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
  // amount of time has elapsed and returns true if the file was
  // successfully removed and false if the file didn't exist (or an
  // error, e.g., permission denied).
  process::Future<bool> schedule(const Duration& d, const std::string& path);

private:
  GarbageCollectorProcess* process;
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __SLAVE_GC_HPP__
