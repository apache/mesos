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

#include "slave/isolator.hpp"
#include "slave/monitor.hpp"

using namespace process;

using process::statistics;

using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using process::wait; // Necessary on some OS's to disambiguate.

// Resource statistics constants.
// These match the names in the ResourceStatistics protobuf.
// TODO(bmahler): Later, when we have a richer monitoring story,
// we will want to publish these outside of this file.
// TODO(cdel): Check if we need any more of the cgroup stats.
const std::string CPUS_TIME_SECS        = "cpus_time_secs";
const std::string CPUS_USER_TIME_SECS   = "cpus_user_time_secs";
const std::string CPUS_SYSTEM_TIME_SECS = "cpus_system_time_secs";
const std::string CPUS_LIMIT            = "cpus_limit";
const std::string MEM_RSS_BYTES         = "mem_rss_bytes";
const std::string MEM_LIMIT_BYTES       = "mem_limit_bytes";
const std::string CPUS_NR_PERIODS       = "cpus_nr_periods";
const std::string CPUS_NR_THROTTLED     = "cpus_nr_throttled";
const std::string CPUS_THROTTLED_TIME_SECS = "cpus_throttled_time_secs";

// TODO(bmahler): Deprecated statistical names, these will be removed!
const std::string CPU_TIME   = "cpu_time";
const std::string CPU_USAGE  = "cpu_usage";
const std::string MEMORY_RSS = "memory_rss";


// Local function prototypes.
void publish(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ResourceStatistics& statistics);

Future<http::Response> _statisticsJSON(
    const hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> >& executors,
    const map<string, double>& statistics,
    const Option<string>& jsonp);

Future<http::Response> _usage(
    const hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> >& executors,
    const map<string, double>& statistics,
    const Option<string>& jsonp);

Future<Nothing> ResourceMonitorProcess::watch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ExecutorInfo& executorInfo,
    const Duration& interval)
{
  if (watches.contains(frameworkId) &&
      watches[frameworkId].contains(executorId)) {
    return Future<Nothing>::failed("Already watched");
  }

  watches[frameworkId][executorId] = executorInfo;

  // Set up the cpu usage meter prior to collecting.
  const string& prefix =
    strings::join("/", frameworkId.value(), executorId.value(), "");

  ::statistics->meter(
      "monitor",
      prefix + CPUS_TIME_SECS,
      new meters::TimeRate(prefix + CPU_USAGE));

  // Schedule the resource collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);

  return Nothing();
}


Future<Nothing> ResourceMonitorProcess::unwatch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  const string& prefix =
    strings::join("/", frameworkId.value(), executorId.value(), "");

  // In case we've already noticed the executor was terminated,
  // we need to archive the statistics first.
  // No need to archive CPUS_USAGE as it is implicitly archived along
  // with CPUS_TIME_SECS.
  ::statistics->archive("monitor", prefix + CPUS_USER_TIME_SECS);
  ::statistics->archive("monitor", prefix + CPUS_SYSTEM_TIME_SECS);
  ::statistics->archive("monitor", prefix + CPUS_LIMIT);
  ::statistics->archive("monitor", prefix + MEM_RSS_BYTES);
  ::statistics->archive("monitor", prefix + MEM_LIMIT_BYTES);
  ::statistics->archive("monitor", prefix + CPUS_NR_PERIODS);
  ::statistics->archive("monitor", prefix + CPUS_NR_THROTTLED);
  ::statistics->archive("monitor", prefix + CPUS_THROTTLED_TIME_SECS);

  if (!watches.contains(frameworkId) ||
      !watches[frameworkId].contains(executorId)) {
    return Future<Nothing>::failed("Not watched");
  }

  watches[frameworkId].erase(executorId);

  if (watches[frameworkId].empty()) {
    watches.erase(frameworkId);
  }

  return Nothing();
}


void ResourceMonitorProcess::collect(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Duration& interval)
{
  // Has the executor been unwatched?
  if (!watches.contains(frameworkId) ||
      !watches[frameworkId].contains(executorId)) {
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
  if (!watches.contains(frameworkId) ||
      !watches[frameworkId].contains(executorId)) {
    return;
  }

  if (statistics.isReady()) {
    // Publish the data to the statistics module.
    VLOG(1) << "Publishing resource usage for executor '" << executorId
            << "' of framework '" << frameworkId << "'";
    publish(frameworkId, executorId, statistics.get());
  } else {
    // Note that the isolator might have been terminated and pending
    // dispatches deleted, causing the future to get discarded.
    VLOG(1)
      << "Failed to collect resource usage for executor '" << executorId
      << "' of framework '" << frameworkId << "': "
      << (statistics.isFailed() ? statistics.failure() : "Future discarded");
  }

  // Schedule the next collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);
}


// TODO(bmahler): With slave recovery, executor uuid's will be exposed
// to the isolator. This means that we will be able to publish
// statistics per executor run, rather than across all runs.
void publish(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ResourceStatistics& statistics)
{
  Try<Time> time_ = Time::create(statistics.timestamp());
  if (time_.isError()) {
    LOG(ERROR) << "Not publishing the statistics because we cannot create a "
               << "Duration from its timestamp: " << time_.error();
    return;
  }

  Time time = time_.get();

  const string& prefix =
    strings::join("/", frameworkId.value(), executorId.value(), "");

  // Publish cpu usage statistics.
  ::statistics->set(
      "monitor",
      prefix + CPUS_USER_TIME_SECS,
      statistics.cpus_user_time_secs(),
      time);
  ::statistics->set(
      "monitor",
      prefix + CPUS_SYSTEM_TIME_SECS,
      statistics.cpus_system_time_secs(),
      time);
  ::statistics->set(
      "monitor",
      prefix + CPUS_LIMIT,
      statistics.cpus_limit(),
      time);
  // The applied meter from watch() will publish the cpu usage.
  ::statistics->set(
      "monitor",
      prefix + CPUS_TIME_SECS,
      statistics.cpus_user_time_secs() + statistics.cpus_system_time_secs(),
      time);

  // Publish memory statistics.
  ::statistics->set(
      "monitor",
      prefix + MEM_RSS_BYTES,
      statistics.mem_rss_bytes(),
      time);
  ::statistics->set(
      "monitor",
      prefix + MEM_LIMIT_BYTES,
      statistics.mem_limit_bytes(),
      time);

  // Publish cpu.stat statistics.
  ::statistics->set(
      "monitor",
      prefix + CPUS_NR_PERIODS,
      statistics.cpus_nr_periods(),
      time);
  ::statistics->set(
      "monitor",
      prefix + CPUS_NR_THROTTLED,
      statistics.cpus_nr_throttled(),
      time);
  ::statistics->set(
      "monitor",
      prefix + CPUS_THROTTLED_TIME_SECS,
      statistics.cpus_throttled_time_secs(),
      time);
}


Future<http::Response> ResourceMonitorProcess::statisticsJSON(
    const http::Request& request)
{
  lambda::function<Future<http::Response>(const map<string, double>&)>
    _statisticsJSON = lambda::bind(
      slave::_statisticsJSON,
      watches,
      lambda::_1,
      request.query.get("jsonp"));

  return ::statistics->get("monitor").then(_statisticsJSON);
}


Future<http::Response> _statisticsJSON(
    const hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> >& watches,
    const map<string, double>& statistics,
    const Option<string>& jsonp)
{
  JSON::Array result;

  foreachkey (const FrameworkID& frameworkId, watches) {
    foreachkey (const ExecutorID& executorId, watches.get(frameworkId).get()) {
      const ExecutorInfo& info =
        watches.get(frameworkId).get().get(executorId).get();
      const string& prefix =
        strings::join("/", frameworkId.value(), executorId.value(), "");

      // Export zero values by default.
      JSON::Object usage;
      usage.values[CPUS_USER_TIME_SECS] = 0;
      usage.values[CPUS_SYSTEM_TIME_SECS] = 0;
      usage.values[CPUS_LIMIT] = 0;
      usage.values[MEM_RSS_BYTES] = 0;
      usage.values[MEM_LIMIT_BYTES] = 0;
      usage.values[CPUS_NR_PERIODS] = 0;
      usage.values[CPUS_NR_THROTTLED] = 0;
      usage.values[CPUS_THROTTLED_TIME_SECS] = 0;

      // Set the cpu usage data if present.
      if (statistics.count(prefix + CPUS_USER_TIME_SECS) > 0) {
        usage.values[CPUS_USER_TIME_SECS] =
          statistics.find(prefix + CPUS_USER_TIME_SECS)->second;
      }
      if (statistics.count(prefix + CPUS_SYSTEM_TIME_SECS) > 0) {
        usage.values[CPUS_SYSTEM_TIME_SECS] =
          statistics.find(prefix + CPUS_SYSTEM_TIME_SECS)->second;
      }
      if (statistics.count(prefix + CPUS_LIMIT) > 0) {
        usage.values[CPUS_LIMIT] = statistics.find(prefix + CPUS_LIMIT)->second;
      }

      // Set the memory usage data if present.
      if (statistics.count(prefix + MEM_RSS_BYTES) > 0) {
        usage.values[MEM_RSS_BYTES] =
          statistics.find(prefix + MEM_RSS_BYTES)->second;
      }
      if (statistics.count(prefix + MEM_LIMIT_BYTES) > 0) {
        usage.values[MEM_LIMIT_BYTES] =
          statistics.find(prefix + MEM_LIMIT_BYTES)->second;
      }

      // Set the cpu.stat data if present.
      if (statistics.count(prefix + CPUS_NR_PERIODS) > 0) {
        usage.values[CPUS_NR_PERIODS] =
          statistics.find(prefix + CPUS_NR_PERIODS)->second;
      }
      if (statistics.count(prefix + CPUS_NR_THROTTLED) > 0) {
        usage.values[CPUS_NR_THROTTLED] =
          statistics.find(prefix + CPUS_NR_THROTTLED)->second;
      }
      if (statistics.count(prefix + CPUS_THROTTLED_TIME_SECS) > 0) {
        usage.values[CPUS_THROTTLED_TIME_SECS] =
          statistics.find(prefix + CPUS_THROTTLED_TIME_SECS)->second;
      }

      JSON::Object entry;
      entry.values["framework_id"] = frameworkId.value();
      entry.values["executor_id"] = executorId.value();
      entry.values["executor_name"] = info.name();
      entry.values["source"] = info.source();
      entry.values["statistics"] = usage;

      result.values.push_back(entry);
    }
  }

  return http::OK(result, jsonp);
}


Future<http::Response> ResourceMonitorProcess::usage(
    const http::Request& request)
{
  lambda::function<Future<http::Response>(const map<string, double>&)>
    _usage = lambda::bind(
      slave::_usage,
      watches,
      lambda::_1,
      request.query.get("jsonp"));

  return ::statistics->get("monitor").then(_usage);
}


Future<http::Response> _usage(
    const hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> >& watches,
    const map<string, double>& statistics,
    const Option<string>& jsonp)
{
  JSON::Array result;

  foreachkey (const FrameworkID& frameworkId, watches) {
    foreachkey (const ExecutorID& executorId, watches.get(frameworkId).get()) {
      const ExecutorInfo& info =
        watches.get(frameworkId).get().get(executorId).get();
      const string& prefix =
        strings::join("/", frameworkId.value(), executorId.value(), "");

      // Export zero values by default.
      JSON::Object usage;
      usage.values[CPU_USAGE] = 0;
      usage.values[CPU_TIME] = 0;
      usage.values[MEMORY_RSS] = 0;

      // Set the usage data if present.
      if (statistics.count(prefix + CPU_USAGE) > 0) {
        usage.values[CPU_USAGE] = statistics.find(prefix + CPU_USAGE)->second;
      }
      if (statistics.count(prefix + CPUS_TIME_SECS) > 0) {
        usage.values[CPU_TIME] =
          statistics.find(prefix + CPUS_TIME_SECS)->second;
      }
      if (statistics.count(prefix + MEM_RSS_BYTES) > 0) {
        usage.values[MEMORY_RSS] =
          statistics.find(prefix + MEM_RSS_BYTES)->second;
      }

      JSON::Object entry;
      entry.values["framework_id"] = frameworkId.value();
      entry.values["executor_id"] = executorId.value();
      entry.values["executor_name"] = info.name();
      entry.values["source"] = info.source();
      entry.values["resource_usage"] = usage;

      result.values.push_back(entry);
    }
  }

  return http::OK(result, jsonp);
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
