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

#include <vector>

#include <mesos/mesos.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/stringify.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "messages/messages.hpp"

using std::vector;

using namespace mesos;

// TODO(bmahler): Add tests for other JSON models.

// This test ensures we don't break the API when it comes to JSON
// representation of tasks. Also, we want to ensure that tasks are
// modeled the same way when using 'Task' vs. 'TaskInfo'.
TEST(HTTP, ModelTask)
{
  TaskID taskId;
  taskId.set_value("t");

  SlaveID slaveId;
  slaveId.set_value("s");

  ExecutorID executorId;
  executorId.set_value("t");

  FrameworkID frameworkId;
  frameworkId.set_value("f");

  TaskState state = TASK_RUNNING;

  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(taskId);
  status.set_state(state);
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.mutable_executor_id()->CopyFrom(executorId);
  status.set_timestamp(0.0);

  statuses.push_back(status);

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->CopyFrom(taskId);
  task.mutable_slave_id()->CopyFrom(slaveId);
  task.mutable_command()->set_value("echo hello");

  Task task_ = protobuf::createTask(task, state, frameworkId);
  task_.add_statuses()->CopyFrom(statuses[0]);

  JSON::Value object = model(task, frameworkId, state, statuses);
  JSON::Value object_ = model(task_);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"executor_id\":\"\","
      "  \"framework_id\":\"f\","
      "  \"id\":\"t\","
      "  \"labels\": [],"
      "  \"name\":\"task\","
      "  \"resources\":"
      "  {"
      "    \"cpus\":0,"
      "    \"disk\":0,"
      "    \"mem\":0"
      "  },"
      "  \"slave_id\":\"s\","
      "  \"state\":\"TASK_RUNNING\","
      "  \"statuses\":"
      "  ["
      "    {"
      "      \"state\":\"TASK_RUNNING\","
      "      \"timestamp\":0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);

  EXPECT_EQ(expected.get(), object);
  EXPECT_EQ(expected.get(), object_);

  // Ensure both are modeled the same.
  EXPECT_EQ(object, object_);
}
