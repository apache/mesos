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

#include <string>

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/limiter.hpp>
#include <process/process.hpp>

#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/protobuf.hpp>

#include "slave/monitor.hpp"

using namespace process;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

static const string STATISTICS_HELP()
{
  return HELP(
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
}


class ResourceMonitorProcess : public Process<ResourceMonitorProcess>
{
public:
  explicit ResourceMonitorProcess(
      const lambda::function<Future<ResourceUsage>()>& _usage)
    : ProcessBase("monitor"),
      usage(_usage),
      limiter(2, Seconds(1)) {} // 2 permits per second.

  virtual ~ResourceMonitorProcess() {}

protected:
  virtual void initialize()
  {
    route("/statistics.json",
          STATISTICS_HELP(),
          &ResourceMonitorProcess::statistics);
  }

private:
  // Returns the monitoring statistics. Requests have no parameters.
  Future<http::Response> statistics(const http::Request& request)
  {
    return limiter.acquire()
      .then(defer(self(), &Self::_statistics, request));
  }

  Future<http::Response> _statistics(const http::Request& request)
  {
    return usage()
      .then(defer(self(), &Self::__statistics, lambda::_1, request));
  }

  Future<http::Response> __statistics(
      const Future<ResourceUsage>& future,
      const http::Request& request)
  {
    if (!future.isReady()) {
      LOG(WARNING) << "Could not collect resource usage: "
                   << (future.isFailed() ? future.failure() : "discarded");

      return http::InternalServerError();
    }

    JSON::Array result;

    foreach (const ResourceUsage::Executor& executor,
             future.get().executors()) {
      if (executor.has_statistics()) {
        const ExecutorInfo info = executor.executor_info();

        JSON::Object entry;
        entry.values["framework_id"] = info.framework_id().value();
        entry.values["executor_id"] = info.executor_id().value();
        entry.values["executor_name"] = info.name();
        entry.values["source"] = info.source();
        entry.values["statistics"] = JSON::Protobuf(executor.statistics());

        result.values.push_back(entry);
      }
    }

    return http::OK(result, request.query.get("jsonp"));
  }

  // Callback used to retrieve resource usage information from slave.
  const lambda::function<Future<ResourceUsage>()> usage;

  // Used to rate limit the statistics.json endpoint.
  RateLimiter limiter;
};


ResourceMonitor::ResourceMonitor(
    const lambda::function<Future<ResourceUsage>()>& usage)
  : process(new ResourceMonitorProcess(usage))
{
  spawn(process.get());
}


ResourceMonitor::~ResourceMonitor()
{
  terminate(process.get());
  wait(process.get());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
