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

#include <vector>

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::vector;

using mesos::internal::protobuf::createLabel;
using mesos::internal::protobuf::createTask;

// TODO(bmahler): Add tests for other JSON models.

// This test ensures we don't break the API when it comes to JSON
// representation of tasks. Also, we want to ensure that tasks are
// modeled the same way when using 'Task' vs. 'TaskInfo'.
TEST(HTTPTest, ModelTask)
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

  Labels labels;
  labels.add_labels()->CopyFrom(createLabel("ACTION", "port:7987 DENY"));

  Ports ports;
  Port* port = ports.add_ports();
  port->set_number(80);
  port->mutable_labels()->CopyFrom(labels);

  DiscoveryInfo discovery;
  discovery.set_visibility(DiscoveryInfo::CLUSTER);
  discovery.set_name("discover");
  discovery.mutable_ports()->CopyFrom(ports);

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->CopyFrom(taskId);
  task.mutable_slave_id()->CopyFrom(slaveId);
  task.mutable_command()->set_value("echo hello");
  task.mutable_discovery()->CopyFrom(discovery);

  Task task_ = createTask(task, state, frameworkId);
  task_.add_statuses()->CopyFrom(statuses[0]);

  JSON::Value object = model(task, frameworkId, state, statuses);
  JSON::Value object_ = model(task_);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"executor_id\":\"\","
      "  \"framework_id\":\"f\","
      "  \"id\":\"t\","
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
      "  ],"
      " \"discovery\":"
      " {"
      "   \"name\":\"discover\","
      "   \"ports\":"
      "   {"
      "     \"ports\":"
      "     ["
      "       {"
      "         \"number\":80,"
      "         \"labels\":"
      "         {"
      "           \"labels\":"
      "           ["
      "             {"
      "              \"key\":\"ACTION\","
      "              \"value\":\"port:7987 DENY\""
      "             }"
      "           ]"
      "         }"
      "       }"
      "     ]"
      "   },"
      "   \"visibility\":\"CLUSTER\""
      " }"
      "}");

  ASSERT_SOME(expected);

  EXPECT_EQ(expected.get(), object);
  EXPECT_EQ(expected.get(), object_);

  // Ensure both are modeled the same.
  EXPECT_EQ(object, object_);
}


// This test verifies that Resources model combines all resources of different
// roles and filters out revocable resources.
TEST(HTTPTest, ModelResources)
{
  // Resources of mixed types, roles, duplicate names; standard (
  // e.g., 'cpus') and custom (i.e., 'bar').
  Resources nonRevocable = Resources::parse(
      "cpus:1;cpus(foo):1;mem:512;disk:1024;ports(foo):[1-10];bar:1").get();

  Resource revocableCpus = Resources::parse("cpus", "1.1", "*").get();
  revocableCpus.mutable_revocable();
  Resource revocableMem = Resources::parse("mem", "513", "*").get();
  revocableMem.mutable_revocable();
  Resource revocableDisk = Resources::parse("disk", "1025", "*").get();
  revocableDisk.mutable_revocable();

  Resources total =
    nonRevocable + revocableCpus + revocableMem + revocableDisk;

  JSON::Value object = model(total);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"bar\":1,"
      "  \"cpus\":2,"
      "  \"cpus_revocable\":1.1,"
      "  \"disk\":1024,"
      "  \"disk_revocable\":1025,"
      "  \"mem\":512,"
      "  \"mem_revocable\":513,"
      "  \"ports\":\"[1-10]\""
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), object);
}


// This test verifies that ResourcesMap model are divided by keys.
TEST(HTTP, ModelRoleResources)
{
  Resources fooResources = Resources::parse(
      "cpus(foo):1;ports(foo):[1-10]").get();
  Resources barResources = Resources::parse(
      "mem(bar):512;disk(bar):1024").get();

  hashmap<std::string, Resources> roleResources;
  roleResources["foo"] = fooResources;
  roleResources["bar"] = barResources;

  JSON::Value object = model(roleResources);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"foo\":"
      "  {"
      "    \"cpus\":1,"
      "    \"disk\":0,"
      "    \"mem\":0,"
      "    \"ports\":\"[1-10]\""
      "  },"
      "  \"bar\":"
      "  {"
      "    \"cpus\":0,"
      "    \"mem\":512,"
      "    \"disk\":1024"
      "  }"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), object);
}

// This test ensures we don't break the API when it comes to JSON
// representation of NetworkInfo.
TEST(HTTP, SerializeNetworkInfo)
{
  NetworkInfo networkInfo;
  NetworkInfo::IPAddress* address = networkInfo.add_ip_addresses();
  address->set_protocol(NetworkInfo::IPv4);
  address->set_ip_address("10.0.0.1");
  networkInfo.set_protocol(NetworkInfo::IPv6);
  networkInfo.set_ip_address("10.0.0.2");
  networkInfo.add_groups("foo");
  networkInfo.add_groups("bar");

  JSON::Value object = JSON::protobuf(networkInfo);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"ip_addresses\":"
      "  ["
      "    {"
      "      \"protocol\": \"IPv4\","
      "      \"ip_address\": \"10.0.0.1\""
      "    }"
      "  ],"
      "  \"protocol\": \"IPv6\","
      "  \"ip_address\": \"10.0.0.2\","
      "  \"groups\": ["
      "    \"foo\","
      "    \"bar\""
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), object);
}
