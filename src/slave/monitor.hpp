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

#ifndef __SLAVE_MONITOR_HPP__
#define __SLAVE_MONITOR_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class IsolationModule;
class ResourceMonitorProcess;


// Provides resource monitoring for executors. Resource usage time
// series are stored using the Statistics module. Usage information
// is also exported via a JSON endpoint.
// TODO(bmahler): Forward usage information to the master.
// TODO(bmahler): Consider pulling out the resource collection into
// a Collector abstraction. The monitor can then become a true
// monitoring abstraction, allowing isolation modules to subscribe
// to resource usage events. (e.g. get a future for the executor
// hitting 75% memory consumption, the future would become ready
// when this occurs, and the isolation module can discard the future
// when no longer interested).
class ResourceMonitor
{
public:
  ResourceMonitor(IsolationModule* isolation);
  ~ResourceMonitor();

  // Starts monitoring resources for the given executor.
  // Returns a failure if the executor is already being watched.
  process::Future<Nothing> watch(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ExecutorInfo& executorInfo,
      const Duration& interval);

  // Stops monitoring resources for the given executor.
  // Returns a failure if the executor is unknown to the monitor.
  process::Future<Nothing> unwatch(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

private:
  ResourceMonitorProcess* process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_MONITOR_HPP__
