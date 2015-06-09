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

#include <limits>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>

#include "slave/monitor.hpp"

using namespace process;

using mesos::internal::slave::ResourceMonitor;

using std::numeric_limits;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

TEST(MonitorTest, Statistics)
{
  FrameworkID frameworkId;
  frameworkId.set_value("framework");

  ExecutorID executorId;
  executorId.set_value("executor");

  ExecutorInfo executorInfo;
  executorInfo.mutable_executor_id()->CopyFrom(executorId);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.set_name("name");
  executorInfo.set_source("source");

  ResourceStatistics statistics;
  statistics.set_cpus_nr_periods(100);
  statistics.set_cpus_nr_throttled(2);
  statistics.set_cpus_user_time_secs(4);
  statistics.set_cpus_system_time_secs(1);
  statistics.set_cpus_throttled_time_secs(0.5);
  statistics.set_cpus_limit(1.0);
  statistics.set_mem_file_bytes(0);
  statistics.set_mem_anon_bytes(0);
  statistics.set_mem_mapped_file_bytes(0);
  statistics.set_mem_rss_bytes(1024);
  statistics.set_mem_limit_bytes(2048);
  statistics.set_timestamp(0);

  ResourceMonitor monitor([=]() -> Future<ResourceUsage> {
    Resources resources = Resources::parse("cpus:1;mem:2").get();

    ResourceUsage usage;
    ResourceUsage::Executor* executor = usage.add_executors();
    executor->mutable_executor_info()->CopyFrom(executorInfo);
    executor->mutable_allocated()->CopyFrom(resources);
    executor->mutable_statistics()->CopyFrom(statistics);

    return usage;
  });

  UPID upid("monitor", address());

  Future<http::Response> response = http::get(upid, "statistics.json");
  AWAIT_READY(response);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);

  // TODO(bmahler): Use JSON equality instead to avoid having to use
  // numeric limits for double precision.
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      strings::format(
          "[{"
              "\"executor_id\":\"executor\","
              "\"executor_name\":\"name\","
              "\"framework_id\":\"framework\","
              "\"source\":\"source\","
              "\"statistics\":{"
                  "\"cpus_limit\":%g,"
                  "\"cpus_nr_periods\":%d,"
                  "\"cpus_nr_throttled\":%d,"
                  "\"cpus_system_time_secs\":%g,"
                  "\"cpus_throttled_time_secs\":%g,"
                  "\"cpus_user_time_secs\":%g,"
                  "\"mem_anon_bytes\":%lu,"
                  "\"mem_file_bytes\":%lu,"
                  "\"mem_limit_bytes\":%lu,"
                  "\"mem_mapped_file_bytes\":%lu,"
                  "\"mem_rss_bytes\":%lu,"
                  "\"timestamp\":"
                      "%." + stringify(numeric_limits<double>::digits10) + "g"
              "}"
          "}]",
          statistics.cpus_limit(),
          statistics.cpus_nr_periods(),
          statistics.cpus_nr_throttled(),
          statistics.cpus_system_time_secs(),
          statistics.cpus_throttled_time_secs(),
          statistics.cpus_user_time_secs(),
          statistics.mem_anon_bytes(),
          statistics.mem_file_bytes(),
          statistics.mem_limit_bytes(),
          statistics.mem_mapped_file_bytes(),
          statistics.mem_rss_bytes(),
          statistics.timestamp()).get(),
      response);
}


// This test verifies the correct handling of the statistics.json
// endpoint when there is no executor running.
TEST(MonitorTest, NoExecutor)
{
  ResourceMonitor monitor([]() -> Future<ResourceUsage> {
    return ResourceUsage();
  });

  UPID upid("monitor", address());

  Future<http::Response> response = http::get(upid, "statistics.json");
  AWAIT_READY(response);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


// This test verifies the correct handling of the statistics.json
// endpoint when statistics is missing in ResourceUsage.
TEST(MonitorTest, MissingStatistics)
{
  ResourceMonitor monitor([]() -> Future<ResourceUsage> {
    FrameworkID frameworkId;
    frameworkId.set_value("framework");

    ExecutorID executorId;
    executorId.set_value("executor");

    ExecutorInfo executorInfo;
    executorInfo.mutable_executor_id()->CopyFrom(executorId);
    executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
    executorInfo.set_name("name");
    executorInfo.set_source("source");

    Resources resources = Resources::parse("cpus:1;mem:2").get();

    ResourceUsage usage;
    ResourceUsage::Executor* executor = usage.add_executors();
    executor->mutable_executor_info()->CopyFrom(executorInfo);
    executor->mutable_allocated()->CopyFrom(resources);

    return usage;
  });

  UPID upid("monitor", address());

  Future<http::Response> response = http::get(upid, "statistics.json");
  AWAIT_READY(response);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
