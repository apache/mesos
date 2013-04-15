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
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class Isolator;
class ResourceMonitorProcess;


// Provides resource monitoring for executors. Resource usage time
// series are stored using the Statistics module. Usage information
// is also exported via a JSON endpoint.
// TODO(bmahler): Forward usage information to the master.
// TODO(bmahler): Consider pulling out the resource collection into
// a Collector abstraction. The monitor can then become a true
// monitoring abstraction, allowing isolators to subscribe
// to resource usage events. (e.g. get a future for the executor
// hitting 75% memory consumption, the future would become ready
// when this occurs, and the isolator can discard the future
// when no longer interested).
class ResourceMonitor
{
public:
  ResourceMonitor(Isolator* isolator);
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


class ResourceMonitorProcess : public process::Process<ResourceMonitorProcess>
{
public:
  ResourceMonitorProcess(Isolator* _isolator)
    : ProcessBase("monitor"), isolator(_isolator) {}

  virtual ~ResourceMonitorProcess() {}

  process::Future<Nothing> watch(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ExecutorInfo& executorInfo,
      const Duration& interval);

  process::Future<Nothing> unwatch(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

protected:
  virtual void initialize()
  {
    route("/usage.json", &ResourceMonitorProcess::usage);
  }

private:
  void collect(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Duration& interval);

  void _collect(
      const process::Future<ResourceStatistics>& statistics,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Duration& interval);

  // Returns the usage information. Requests have no parameters.
  process::Future<process::http::Response> usage(
      const process::http::Request& request);

  Isolator* isolator;

  // The executor info is stored for each watched executor.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> > watches;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_MONITOR_HPP__
