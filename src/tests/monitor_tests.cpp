// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <limits>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>

#include "slave/constants.hpp"
#include "slave/monitor.hpp"

#include "tests/mesos.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::ResourceMonitor;
using mesos::internal::slave::Slave;

using std::numeric_limits;
using std::vector;

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

  UPID upid("monitor", process::address());

  Future<http::Response> response = http::get(upid, "statistics");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  JSON::Array expected;
  JSON::Object usage;
  usage.values["executor_id"] = "executor";
  usage.values["executor_name"] = "name";
  usage.values["framework_id"] = "framework";
  usage.values["source"] = "source";
  usage.values["statistics"] = JSON::protobuf(statistics);
  expected.values.push_back(usage);

  Try<JSON::Array> result = JSON::parse<JSON::Array>(response.get().body);
  ASSERT_SOME(result);
  ASSERT_EQ(expected, result.get());
}


// This test verifies the correct handling of the statistics
// endpoint when there is no executor running.
TEST(MonitorTest, NoExecutor)
{
  ResourceMonitor monitor([]() -> Future<ResourceUsage> {
    return ResourceUsage();
  });

  UPID upid("monitor", process::address());

  Future<http::Response> response = http::get(upid, "statistics");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


// This test verifies the correct handling of the statistics
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

  UPID upid("monitor", process::address());

  Future<http::Response> response = http::get(upid, "statistics");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


class MonitorIntegrationTest : public MesosTest {};


// This is an end-to-end test that verifies that the slave returns the
// correct ResourceUsage based on the currently running executors, and
// the values returned by the statistics endpoint are as expected.
TEST_F(MonitorIntegrationTest, RunningExecutor)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  const Offer& offer = offers.get()[0];

  // Launch a task and wait until it is in RUNNING status.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      "sleep 1000");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Hit the statistics endpoint and expect the response contains the
  // resource statistics for the running container.
  UPID upid("monitor", process::address());

  Future<http::Response> response = http::get(upid, "statistics");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  // Verify that the statistics in the response contains the proper
  // resource limits for the container.
  Try<JSON::Value> value = JSON::parse(response.get().body);
  ASSERT_SOME(value);

  Try<JSON::Value> expected = JSON::parse(strings::format(
      "[{"
          "\"statistics\":{"
              "\"cpus_limit\":%g,"
              "\"mem_limit_bytes\":%lu"
          "}"
      "}]",
      1 + slave::DEFAULT_EXECUTOR_CPUS,
      (Megabytes(32) + slave::DEFAULT_EXECUTOR_MEM).bytes()).get());

  ASSERT_SOME(expected);
  EXPECT_TRUE(value.get().contains(expected.get()));

  driver.stop();
  driver.join();

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
