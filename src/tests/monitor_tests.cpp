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
#include <map>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>

#include "slave/constants.hpp"
#include "slave/monitor.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using process::Clock;
using process::Future;

using process::http::BadRequest;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;

using std::numeric_limits;
using std::string;

using testing::_;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


TEST(MonitorTest, Statistics)
{
  FrameworkID frameworkId;
  frameworkId.set_value("framework");

  ExecutorID executorId;
  executorId.set_value("executor");

  ContainerID containerId;
  containerId.set_value("container");

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

  TestContainerizer containerizer;

  Future<Nothing> usage;
  EXPECT_CALL(containerizer, usage(containerId))
    .WillOnce(DoAll(FutureSatisfy(&usage),
                    Return(statistics)));

  slave::ResourceMonitor monitor(&containerizer);

  // We pause the clock first to ensure unexpected collections
  // are avoided.
  process::Clock::pause();

  monitor.start(
      containerId,
      executorInfo);

  // Now wait for ResouorceMonitorProcess::watch to finish.
  process::Clock::settle();

  process::UPID upid("monitor", process::address());

  // Request the statistics, this will ask the isolator.
  Future<Response> response = process::http::get(upid, "statistics.json");

  AWAIT_READY(response);

  // The collection should have occurred on the isolator.
  ASSERT_TRUE(usage.isReady());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
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

  // Ensure the monitor stops polling the isolator.
  monitor.stop(containerId);

  // Wait until ResourceMonitorProcess::stop has completed.
  process::Clock::settle();

  // This time, Containerizer::usage should not get called.
  EXPECT_CALL(containerizer, usage(containerId))
    .Times(0);

  response = process::http::get(upid, "statistics.json");

  // Ensure the rate limiter acquires its permit.
  process::Clock::advance(slave::RESOURCE_MONITORING_INTERVAL);
  process::Clock::settle();

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


// Test for correct handling of the statistics.json endpoint when
// monitoring of a container is stopped.
TEST(MonitorTest, UsageFailure)
{
  TestContainerizer containerizer;

  // Test containerizer is set up to:
  // 1) Synchronize test with Containerizer::usage()
  // 2) After that, stop monitoring the container.
  Future<Nothing> usage;
  process::Promise<ResourceStatistics> failPromise;
  EXPECT_CALL(containerizer, usage(DEFAULT_CONTAINER_ID))
    .WillOnce(DoAll(FutureSatisfy(&usage),
                    Return(failPromise.future())));

  slave::ResourceMonitor monitor(&containerizer);

  AWAIT_READY(monitor.start(DEFAULT_CONTAINER_ID, DEFAULT_EXECUTOR_INFO));

  // Induce a call to usage().
  process::UPID upid("monitor", process::address());
  Future<Response> response = process::http::get(upid, "statistics.json");

  // Usage was called, but Future<ResourceStatistics> is still
  // unsatisfied and monitor is blocked.
  AWAIT_READY(usage);

  // Stop monitoring the container.
  AWAIT_READY(monitor.stop(DEFAULT_CONTAINER_ID));

  // Fail the future to the collected container statistic.
  failPromise.set(process::Failure("Injected failure"));

  // Verify an empty response.
  AWAIT_READY(response);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
