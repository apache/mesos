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

#include <map>
#include <string>

#include <boost/circular_buffer.hpp>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/future.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/statistics.hpp>

#include <stout/cache.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class Containerizer;
class ResourceMonitorProcess;


const extern Duration MONITORING_TIME_SERIES_WINDOW;
const extern size_t MONITORING_TIME_SERIES_CAPACITY;


// Provides resource monitoring for containers. Usage information is
// also exported via a JSON endpoint.
// TODO(bmahler): Forward usage information to the master.
class ResourceMonitor
{
public:
  explicit ResourceMonitor(Containerizer* containerizer);
  ~ResourceMonitor();

  // Starts monitoring resources for the given container.
  // Returns a failure if the container is already being watched.
  process::Future<Nothing> start(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo);

  // Stops monitoring resources for the given container.
  // Returns a failure if the container is unknown to the monitor.
  process::Future<Nothing> stop(
      const ContainerID& containerId);

  process::Future<std::list<ResourceUsage>> usages();

private:
  ResourceMonitorProcess* process;
};


class ResourceMonitorProcess : public process::Process<ResourceMonitorProcess>
{
public:
  explicit ResourceMonitorProcess(Containerizer* _containerizer)
    : ProcessBase("monitor"),
      containerizer(_containerizer),
      limiter(2, Seconds(1)) {} // 2 permits per second.

  virtual ~ResourceMonitorProcess() {}

  process::Future<Nothing> start(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo);

  process::Future<Nothing> stop(
      const ContainerID& containerId);

  process::Future<std::list<ResourceUsage>> usages();

protected:
  virtual void initialize()
  {
    route("/statistics.json",
          STATISTICS_HELP,
          &ResourceMonitorProcess::statistics);
  }

private:
  // Helper for returning the usage for a particular executor.
  process::Future<ResourceUsage> usage(ContainerID containerId);

  ResourceUsage _usage(
    const ExecutorInfo& executorInfo,
    const ResourceStatistics& statistics);

  std::list<ResourceUsage> _usages(
      std::list<process::Future<ResourceUsage>> future);

  // HTTP Endpoints.
  // Returns the monitoring statistics. Requests have no parameters.
  process::Future<process::http::Response> statistics(
      const process::http::Request& request);
  process::Future<process::http::Response> _statistics(
      const process::http::Request& request);
  process::Future<process::http::Response> __statistics(
      const process::Future<std::list<ResourceUsage>>& futures,
      const process::http::Request& request);

  static const std::string STATISTICS_HELP;

  Containerizer* containerizer;

  // Used to rate limit the statistics.json endpoint.
  process::RateLimiter limiter;

  // The executor info is stored for each monitored container.
  hashmap<ContainerID, ExecutorInfo> monitored;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_MONITOR_HPP__
