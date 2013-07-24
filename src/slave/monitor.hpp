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

#include <boost/circular_buffer.hpp>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/statistics.hpp>

#include <stout/cache.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class Isolator;
class ResourceMonitorProcess;


const extern Duration MONITORING_TIME_SERIES_WINDOW;
const extern size_t MONITORING_TIME_SERIES_CAPACITY;

// Number of time series to maintain for completed executors.
const extern size_t MONITORING_ARCHIVED_TIME_SERIES;


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
    : ProcessBase("monitor"),
      isolator(_isolator),
      archive(MONITORING_ARCHIVED_TIME_SERIES) {}

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
    route("/statistics.json", None(), &ResourceMonitorProcess::statisticsJSON);

    // TODO(bmahler): Add a archive.json endpoint that exposes
    // historical information, once we have path parameters for
    // routes.
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

  // Returns the monitoring statistics. Requests have no parameters.
  process::Future<process::http::Response> statisticsJSON(
      const process::http::Request& request);

  Isolator* isolator;

  // Monitoring information for an executor.
  struct MonitoringInfo {
    // boost::circular_buffer needs a default constructor.
    MonitoringInfo() {}

    MonitoringInfo(const ExecutorInfo& _executorInfo,
                   const Duration& window,
                   size_t capacity)
      : executorInfo(_executorInfo), statistics(window, capacity) {}

    ExecutorInfo executorInfo;   // Non-const for assignability.
    process::TimeSeries<ResourceStatistics> statistics;
  };

  hashmap<FrameworkID, hashmap<ExecutorID, MonitoringInfo> > executors;

  // Fixed-size history of monitoring information.
  boost::circular_buffer<MonitoringInfo> archive;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_MONITOR_HPP__
