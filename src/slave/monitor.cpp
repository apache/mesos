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

#include "slave/containerizer/containerizer.hpp"
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


Future<Nothing> ResourceMonitorProcess::start(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo)
{
  if (monitored.contains(containerId)) {
    return Failure("Already monitored");
  }

  monitored[containerId] = executorInfo;

  return Nothing();
}


Future<Nothing> ResourceMonitorProcess::stop(
    const ContainerID& containerId)
{
  if (!monitored.contains(containerId)) {
    return Failure("Not monitored");
  }

  monitored.erase(containerId);

  return Nothing();
}


Future<list<ResourceMonitor::Usage>> ResourceMonitorProcess::usages()
{
  list<Future<ResourceMonitor::Usage>> futures;

  foreachkey (const ContainerID& containerId, monitored) {
    futures.push_back(usage(containerId));
  }

  return await(futures)
    .then(defer(self(), &ResourceMonitorProcess::_usages, lambda::_1));
}


list<ResourceMonitor::Usage> ResourceMonitorProcess::_usages(
    list<Future<ResourceMonitor::Usage>> futures)
{
  list<ResourceMonitor::Usage> result;
  foreach(const Future<ResourceMonitor::Usage>& future, futures) {
    if (future.isReady()) {
      result.push_back(future.get());
    }
  }

  return result;
}


Future<ResourceMonitor::Usage> ResourceMonitorProcess::usage(
    ContainerID containerId)
{
  if (!monitored.contains(containerId)) {
    return Failure("Not monitored");
  }

  ExecutorInfo executorInfo = monitored[containerId];

  return containerizer->usage(containerId)
    .then(defer(
        self(),
        &ResourceMonitorProcess::_usage,
        containerId,
        executorInfo,
        lambda::_1))
    .onFailed([&containerId, &executorInfo](const string& failure) {
      LOG(WARNING) << "Failed to get resource usage for "
                   << " container " << containerId
                   << " for executor " << executorInfo.executor_id()
                   << " of framework " << executorInfo.framework_id()
                   << ": " << failure;
    })
    .onDiscarded([&containerId, &executorInfo]() {
      LOG(WARNING) << "Failed to get resource usage for "
                   << " container " << containerId
                   << " for executor " << executorInfo.executor_id()
                   << " of framework " << executorInfo.framework_id()
                   << ": future discarded";
    });
}


ResourceMonitor::Usage ResourceMonitorProcess::_usage(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const ResourceStatistics& statistics)
{
  ResourceMonitor::Usage usage;
  usage.containerId = containerId;
  usage.executorInfo = executorInfo;
  usage.statistics = statistics;

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
  return usages()
    .then(defer(self(), &Self::__statistics, lambda::_1, request));
}


Future<http::Response> ResourceMonitorProcess::__statistics(
    const Future<list<ResourceMonitor::Usage>>& futures,
    const http::Request& request)
{
  if (!futures.isReady()) {
    LOG(WARNING) << "Could not collect usage statistics";
    return http::InternalServerError();
  }

  JSON::Array result;

  foreach (const ResourceMonitor::Usage& usage, futures.get()) {
    JSON::Object entry;
    entry.values["framework_id"] = usage.executorInfo.framework_id().value();
    entry.values["executor_id"] = usage.executorInfo.executor_id().value();
    entry.values["executor_name"] = usage.executorInfo.name();
    entry.values["source"] = usage.executorInfo.source();
    entry.values["statistics"] = JSON::Protobuf(usage.statistics);

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
        "Returns the current resource consumption data for containers",
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


ResourceMonitor::ResourceMonitor(Containerizer* containerizer)
{
  process = new ResourceMonitorProcess(containerizer);
  spawn(process);
}


ResourceMonitor::~ResourceMonitor()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> ResourceMonitor::start(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo)
{
  return dispatch(
      process,
      &ResourceMonitorProcess::start,
      containerId,
      executorInfo);
}


Future<Nothing> ResourceMonitor::stop(
    const ContainerID& containerId)
{
  return dispatch(process, &ResourceMonitorProcess::stop, containerId);
}


Future<list<ResourceMonitor::Usage>> ResourceMonitor::usages()
{
  return dispatch(process, &ResourceMonitorProcess::usages);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
