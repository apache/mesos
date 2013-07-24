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

#include <mesos/mesos.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/protobuf.hpp>

#include "slave/isolator.hpp"
#include "slave/monitor.hpp"

using namespace process;

using std::make_pair;
using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using process::wait; // Necessary on some OS's to disambiguate.


// TODO(bmahler): Consider exposing these as flags should the
// need arise. These are conservative for the initial version.
const Duration MONITORING_TIME_SERIES_WINDOW = Weeks(2);
const size_t MONITORING_TIME_SERIES_CAPACITY = 1000;
const size_t MONITORING_ARCHIVED_TIME_SERIES = 25;


Future<Nothing> ResourceMonitorProcess::watch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ExecutorInfo& executorInfo,
    const Duration& interval)
{
  if (executors.contains(frameworkId) &&
      executors[frameworkId].contains(executorId)) {
    return Failure("Already watched");
  }

  executors[frameworkId][executorId] =
      MonitoringInfo(executorInfo,
                     MONITORING_TIME_SERIES_WINDOW,
                     MONITORING_TIME_SERIES_CAPACITY);

  // Schedule the resource collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);

  return Nothing();
}


Future<Nothing> ResourceMonitorProcess::unwatch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!executors.contains(frameworkId) ||
      !executors[frameworkId].contains(executorId)) {
    return Failure("Not watched");
  }

  // Add the monitoring information to the archive.
  archive.push_back(executors[frameworkId][executorId]);
  executors[frameworkId].erase(executorId);

  if (executors[frameworkId].empty()) {
    executors.erase(frameworkId);
  }

  return Nothing();
}


void ResourceMonitorProcess::collect(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Duration& interval)
{
  // Has the executor been unwatched?
  if (!executors.contains(frameworkId) ||
      !executors[frameworkId].contains(executorId)) {
    return;
  }

  dispatch(isolator, &Isolator::usage, frameworkId, executorId)
    .onAny(defer(self(),
                 &Self::_collect,
                 lambda::_1,
                 frameworkId,
                 executorId,
                 interval));
}


void ResourceMonitorProcess::_collect(
    const Future<ResourceStatistics>& statistics,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Duration& interval)
{
  // Has the executor been unwatched?
  if (!executors.contains(frameworkId) ||
      !executors[frameworkId].contains(executorId)) {
    return;
  }

  if (statistics.isDiscarded()) {
    // Note that the isolator may have terminated, causing pending
    // dispatches to be deleted.
    VLOG(1) << "Ignoring discarded future collecting resource usage for "
            << "executor '" << executorId
            << "' of framework '" << frameworkId << "'";
  } else if (statistics.isFailed()) {
    // TODO(bmahler): Have the Isolators discard the result when the
    // executor was killed or completed.
    VLOG(1)
      << "Failed to collect resource usage for executor '" << executorId
      << "' of framework '" << frameworkId << "': " << statistics.failure();
  } else {
    Try<Time> time = Time::create(statistics.get().timestamp());

    if (time.isError()) {
      LOG(ERROR) << "Invalid timestamp " << statistics.get().timestamp()
                 << " for executor '" << executorId
                 << "' of framework '" << frameworkId << ": " << time.error();
    } else {
      // Add the statistics to the time series.
      executors[frameworkId][executorId].statistics.set(
          statistics.get(), time.get());
    }
  }

  // Schedule the next collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);
}


Future<http::Response> ResourceMonitorProcess::statisticsJSON(
    const http::Request& request)
{
  JSON::Array result;

  foreachkey (const FrameworkID& frameworkId, executors) {
    foreachkey (const ExecutorID& executorId, executors[frameworkId]) {
      const TimeSeries<ResourceStatistics>& timeseries =
        executors[frameworkId][executorId].statistics;

      if (timeseries.empty()) {
        continue;
      }

      const ExecutorInfo& executorInfo =
        executors[frameworkId][executorId].executorInfo;

      JSON::Object entry;
      entry.values["framework_id"] = frameworkId.value();
      entry.values["executor_id"] = executorId.value();
      entry.values["executor_name"] = executorInfo.name();
      entry.values["source"] = executorInfo.source();

      const ResourceStatistics& statistics = timeseries.latest().get().second;
      entry.values["statistics"] = JSON::Protobuf(statistics);

      result.values.push_back(entry);
    }
  }

  return http::OK(result, request.query.get("jsonp"));
}


ResourceMonitor::ResourceMonitor(Isolator* isolator)
{
  process = new ResourceMonitorProcess(isolator);
  spawn(process);
}


ResourceMonitor::~ResourceMonitor()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> ResourceMonitor::watch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ExecutorInfo& executorInfo,
    const Duration& interval)
{
  return dispatch(
      process,
      &ResourceMonitorProcess::watch,
      frameworkId,
      executorId,
      executorInfo,
      interval);
}


Future<Nothing> ResourceMonitor::unwatch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return dispatch(
      process, &ResourceMonitorProcess::unwatch, frameworkId, executorId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
