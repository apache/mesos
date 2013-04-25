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

  ResourceStatistics statistics;
  statistics.set_cpu_user_time(5);
  statistics.set_cpu_system_time(1);
  statistics.set_memory_rss(1024);
  statistics.set_timestamp(Clock::now().secs());

  TestingIsolator isolator;

  process::spawn(isolator);

  EXPECT_CALL(isolator, usage(frameworkId, executorId))
    .WillRepeatedly(Return(statistics));

  slave::ResourceMonitor monitor(&isolator);

  // Monitor the executor.
  Future<Nothing> watch =
    FUTURE_DISPATCH(_, &slave::ResourceMonitorProcess::watch);

  monitor.watch(
      frameworkId,
      executorId,
      executorInfo,
      slave::RESOURCE_MONITORING_INTERVAL);

  AWAIT_READY(watch);

  process::Clock::pause();
  process::Clock::advance(slave::RESOURCE_MONITORING_INTERVAL);
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
          statistics.cpu_user_time() + statistics.cpu_system_time(),
          statistics.cpu_usage(),
          statistics.memory_rss()).get(),
      response);

  // Ensure the monitor stops polling the isolator.
  Future<Nothing> unwatch =
    FUTURE_DISPATCH(_, &slave::ResourceMonitorProcess::unwatch);

  monitor.unwatch(frameworkId, executorId);

  AWAIT_READY(unwatch);

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
