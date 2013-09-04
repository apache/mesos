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

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>

#include "slave/constants.hpp"
#include "slave/monitor.hpp"

#include "tests/isolator.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using process::Clock;
using process::Future;

using process::http::BadRequest;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;

using std::string;

using testing::_;
using testing::DoAll;
using testing::Return;


// TODO(bmahler): Add additional tests:
//   1. Check that the data has been published to statistics.
//   2. Check that metering is occurring on subsequent resource data.
TEST(MonitorTest, WatchUnwatch)
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

  ResourceStatistics initialStatistics;
  initialStatistics.set_cpus_user_time_secs(0);
  initialStatistics.set_cpus_system_time_secs(0);
  initialStatistics.set_cpus_limit(2.5);
  initialStatistics.set_mem_rss_bytes(0);
  initialStatistics.set_mem_limit_bytes(2048);
  initialStatistics.set_timestamp(Clock::now().secs());

  ResourceStatistics statistics;
  statistics.set_cpus_nr_periods(100);
  statistics.set_cpus_nr_throttled(2);
  statistics.set_cpus_user_time_secs(4);
  statistics.set_cpus_system_time_secs(1);
  statistics.set_cpus_throttled_time_secs(0.5);
  statistics.set_cpus_limit(2.5);
  statistics.set_mem_rss_bytes(1024);
  statistics.set_mem_limit_bytes(2048);
  statistics.set_timestamp(
      initialStatistics.timestamp() +
      slave::RESOURCE_MONITORING_INTERVAL.secs());

  TestingIsolator isolator;

  process::spawn(isolator);

  Future<Nothing> usage1, usage2;
  EXPECT_CALL(isolator, usage(frameworkId, executorId))
    .WillOnce(DoAll(FutureSatisfy(&usage1),
                    Return(initialStatistics)))
    .WillOnce(DoAll(FutureSatisfy(&usage2),
                    Return(statistics)));
  slave::ResourceMonitor monitor(&isolator);

  // We pause the clock first in order to make sure that we can
  // advance time below to force the 'delay' in
  // ResourceMonitorProcess::watch to execute.
  process::Clock::pause();

  monitor.watch(
      frameworkId,
      executorId,
      executorInfo,
      slave::RESOURCE_MONITORING_INTERVAL);

  // Now wait for ResouorceMonitorProcess::watch to finish so we can
  // advance time to cause collection to begin.
  process::Clock::settle();

  process::Clock::advance(slave::RESOURCE_MONITORING_INTERVAL);
  process::Clock::settle();

  AWAIT_READY(usage1);

  // Wait until the isolator has finished returning the statistics.
  process::Clock::settle();

  // The second collection will populate the cpus_usage.
  process::Clock::advance(slave::RESOURCE_MONITORING_INTERVAL);
  process::Clock::settle();

  AWAIT_READY(usage2);

  // Wait until the isolator has finished returning the statistics.
  process::Clock::settle();

  process::UPID upid("monitor", process::ip(), process::port());

  Future<Response> response = process::http::get(upid, "usage.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);

  // TODO(bmahler): Verify metering directly through statistics.
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      strings::format(
          "[{"
              "\"executor_id\":\"executor\","
              "\"executor_name\":\"name\","
              "\"framework_id\":\"framework\","
              "\"resource_usage\":{"
                  "\"cpu_time\":%g,"
                  "\"cpu_usage\":%g,"
                  "\"memory_rss\":%lu"
              "},"
              "\"source\":\"source\""
          "}]",
          statistics.cpus_system_time_secs() + statistics.cpus_user_time_secs(),
          (statistics.cpus_system_time_secs() +
           statistics.cpus_user_time_secs()) /
               slave::RESOURCE_MONITORING_INTERVAL.secs(),
          statistics.mem_rss_bytes()).get(),
      response);

  response = process::http::get(upid, "statistics.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);

  // TODO(bmahler): Verify metering directly through statistics.
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
                  "\"mem_limit_bytes\":%lu,"
                  "\"mem_rss_bytes\":%lu"
              "}"
          "}]",
          statistics.cpus_limit(),
          statistics.cpus_nr_periods(),
          statistics.cpus_nr_throttled(),
          statistics.cpus_system_time_secs(),
          statistics.cpus_throttled_time_secs(),
          statistics.cpus_user_time_secs(),
          statistics.mem_limit_bytes(),
          statistics.mem_rss_bytes()).get(),
      response);

  // Ensure the monitor stops polling the isolator.
  monitor.unwatch(frameworkId, executorId);

  // Wait until ResourceMonitorProcess::unwatch has completed.
  process::Clock::settle();

  // This time, Isolator::usage should not get called.
  EXPECT_CALL(isolator, usage(frameworkId, executorId))
    .Times(0);

  process::Clock::advance(slave::RESOURCE_MONITORING_INTERVAL);
  process::Clock::settle();

  response = process::http::get(upid, "usage.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/json",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}
