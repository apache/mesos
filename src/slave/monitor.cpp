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

#include <list>
#include <map>
#include <string>

#include <mesos/mesos.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/statistics.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/protobuf.hpp>

#include "slave/isolator.hpp"
#include "slave/monitor.hpp"

using namespace process;

using std::list;
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


ResourceMonitorProcess::Usage ResourceMonitorProcess::usage(
    const FrameworkID& frameworkId,
    const ExecutorInfo& executorInfo)
{
  Usage usage;
  usage.frameworkId = frameworkId;
  usage.executorInfo = executorInfo;
  usage.statistics = dispatch(
      isolator, &Isolator::usage, frameworkId, executorInfo.executor_id());

  return usage;
}


Future<http::Response> ResourceMonitorProcess::statistics(
    const http::Request& request)
{
  return limiter.acquire()
    .then(defer(self(), &Self::_statistics, request));
}


Future<http::Response> ResourceMonitorProcess::_statistics(
    const http::Request& request)
{
  list<Usage> usages;
  list<Future<ResourceStatistics> > futures;

  foreachkey (const FrameworkID& frameworkId, executors) {
    foreachvalue (const MonitoringInfo& info, executors[frameworkId]) {
      // TODO(bmahler): Consider a batch usage API on the Isolator.
      usages.push_back(usage(frameworkId, info.executorInfo));
      futures.push_back(usages.back().statistics);
    }
  }

  return process::await(futures)
    .then(defer(self(), &Self::__statistics, usages, request));
}


Future<http::Response> ResourceMonitorProcess::__statistics(
    const list<ResourceMonitorProcess::Usage>& usages,
    const http::Request& request)
{
  JSON::Array result;

  foreach (const Usage& usage, usages) {
    if (usage.statistics.isFailed()) {
      LOG(WARNING) << "Failed to get resource usage for executor "
                   << usage.executorInfo.executor_id()
                   << " of framework " << usage.frameworkId
                   << ": " << usage.statistics.failure();
      continue;
    } else if (usage.statistics.isDiscarded()) {
      continue;
    }

    JSON::Object entry;
    entry.values["framework_id"] = usage.frameworkId.value();
    entry.values["executor_id"] = usage.executorInfo.executor_id().value();
    entry.values["executor_name"] = usage.executorInfo.name();
    entry.values["source"] = usage.executorInfo.source();
    entry.values["statistics"] = JSON::Protobuf(usage.statistics.get());

    result.values.push_back(entry);
  }

  return http::OK(result, request.query.get("jsonp"));
}


const string ResourceMonitorProcess::STATISTICS_HELP = HELP(
    TLDR(
        "Retrieve resource monitoring information."),
    USAGE(
        "/statistics.json"),
    DESCRIPTION(
        "Returns the current resource consumption data for executors",
        "running under this slave.",
        "",
        "Example:",
        "",
        "```",
        "[{",
        "    \"executor_id\":\"executor\",",
        "    \"executor_name\":\"name\",",
        "    \"framework_id\":\"framework\",",
        "    \"source\":\"source\",",
        "    \"statistics\":",
        "    {",
        "        \"cpus_limit\":8.25,",
        "        \"cpus_nr_periods\":769021,",
        "        \"cpus_nr_throttled\":1046,",
        "        \"cpus_system_time_secs\":34501.45,",
        "        \"cpus_throttled_time_secs\":352.597023453,",
        "        \"cpus_user_time_secs\":96348.84,",
        "        \"mem_anon_bytes\":4845449216,",
        "        \"mem_file_bytes\":260165632,",
        "        \"mem_limit_bytes\":7650410496,",
        "        \"mem_mapped_file_bytes\":7159808,",
        "        \"mem_rss_bytes\":5105614848,",
        "        \"timestamp\":1388534400.0",
        "    }",
        "}]",
        "```"));


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
