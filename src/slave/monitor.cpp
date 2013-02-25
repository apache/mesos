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

#include "slave/isolation_module.hpp"
#include "slave/monitor.hpp"

using namespace process;

using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using process::wait; // Necessary on some OS's to disambiguate.

// Resource statistics constants.
// These match the names in the ResourceStatistics protobuf.
// TODO(bmahler): Later, when we have a richer monitoring story,
// we will want to publish these outisde of this file.
const std::string CPU_TIME   = "cpu_time";
const std::string CPU_USAGE  = "cpu_usage";
const std::string MEMORY_RSS = "memory_rss";


// Local function prototypes.
void publish(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ResourceStatistics& statistics);

Future<http::Response> _usage(
    const hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> >& executors,
    const map<string, double>& statistics,
    const Option<string>& jsonp);

class ResourceMonitorProcess : public Process<ResourceMonitorProcess>
{
public:
  ResourceMonitorProcess(IsolationModule* _isolation)
    : ProcessBase("monitor"), isolation(_isolation) {}

  virtual ~ResourceMonitorProcess() {}

  Future<Nothing> watch(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ExecutorInfo& executorInfo,
      const Duration& interval);

  Future<Nothing> unwatch(
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
      const Future<ResourceStatistics>& statistics,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Duration& interval);

  // Returns the usage information. Requests have no parameters.
  Future<http::Response> usage(const http::Request& request);

  IsolationModule* isolation;

  // The executor info is stored for each watched executor.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> > watches;
};


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

  process::statistics->meter(
      "monitor",
      prefix + CPU_TIME,
      new meters::TimeRate(prefix + CPU_USAGE));

  // Schedule the resource collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);

  return Nothing();
}


Future<Nothing> ResourceMonitorProcess::unwatch(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
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

  isolation->usage(frameworkId, executorId).onAny(
      defer(self(),
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
  CHECK(!statistics.isDiscarded());

  // Has the executor been unwatched?
  if (!watches.contains(frameworkId) ||
      !watches[frameworkId].contains(executorId)) {
    return;
  }

  if (statistics.isReady()) {
    // Publish the data to the statistics module.
    publish(frameworkId, executorId, statistics.get());
  } else {
    LOG(WARNING)
      << "Failed to collect resources for executor " << executorId
      << " of framework " << frameworkId << ": " << statistics.failure();
  }

  // Schedule the next collection.
  delay(interval, self(), &Self::collect, frameworkId, executorId, interval);
}


// TODO(bmahler): With slave recovery, executor uuid's will be exposed
// to the isolation module. This means that we will be able to publish
// statistics per executor run, rather than across all runs.
void publish(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ResourceStatistics& statistics)
{
  Seconds time(statistics.timestamp());

  const string& prefix =
    strings::join("/", frameworkId.value(), executorId.value(), "");

  // Publish memory statistic.
  process::statistics->set(
      "monitor",
      prefix + MEMORY_RSS,
      statistics.memory_rss(),
      time);

  // Publish cpu usage statistics. The applied meter from watch()
  // will publish the cpu usage percentage.
  process::statistics->set(
      "monitor",
      prefix + CPU_TIME,
      statistics.cpu_user_time() + statistics.cpu_system_time(),
      time);
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

  return process::statistics->get("monitor").then(_usage);
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
      if (statistics.count(prefix + CPU_TIME) > 0) {
        usage.values[CPU_TIME] = statistics.find(prefix + CPU_TIME)->second;
      }
      if (statistics.count(prefix + MEMORY_RSS) > 0) {
        usage.values[MEMORY_RSS] = statistics.find(prefix + MEMORY_RSS)->second;
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


ResourceMonitor::ResourceMonitor(IsolationModule* isolation)
{
  process = new ResourceMonitorProcess(isolation);
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
