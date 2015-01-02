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

#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"
#include "tests/mesos.hpp"

using namespace mesos;

using mesos::master::Master;
using mesos::slave::Slave;

class MetricsTest : public mesos::tests::MesosTest {};

TEST_F(MetricsTest, Master)
{
  Try<process::PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, stats.values.count("master/elected"));

  EXPECT_EQ(1u, stats.values.count("master/slaves_connected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_active"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/frameworks_connected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, stats.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_error"));

  EXPECT_EQ(1u, stats.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, stats.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, stats.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, stats.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_decline_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_framework_to_executor"));

  // Messages from slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, stats.values.count("master/messages_exited_executor"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_framework_to_executor_messages"));

  EXPECT_EQ(1u, stats.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_status_update_acknowledgements"));

  EXPECT_EQ(1u, stats.values.count("master/recovery_slave_removals"));

  EXPECT_EQ(1u, stats.values.count("master/event_queue_messages"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_dispatches"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_http_requests"));

  EXPECT_EQ(1u, stats.values.count("master/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("master/mem_total"));
  EXPECT_EQ(1u, stats.values.count("master/mem_used"));
  EXPECT_EQ(1u, stats.values.count("master/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("master/disk_total"));
  EXPECT_EQ(1u, stats.values.count("master/disk_used"));
  EXPECT_EQ(1u, stats.values.count("master/disk_percent"));

  EXPECT_EQ(1u, stats.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, stats.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, stats.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, stats.values.count("registrar/state_store_ms"));
}


TEST_F(MetricsTest, Slave)
{
  // TODO(dhamon): https://issues.apache.org/jira/browse/MESOS-2134 to allow
  // only a Slave to be started.
  Try<process::PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<process::PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, stats.values.count("slave/registered"));

  EXPECT_EQ(1u, stats.values.count("slave/recovery_errors"));

  EXPECT_EQ(1u, stats.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, stats.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_lost"));

  EXPECT_EQ(1u, stats.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_running"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminated"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(1u, stats.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/mem_total"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_used"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/disk_total"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_used"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_percent"));
}
