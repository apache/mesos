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

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal::tests;


TEST(TypeUtilsTest, TaskGroupEquality)
{
  SlaveID slaveId;
  slaveId.set_value("default-agent");

  Resources resources = Resources::parse("cpus:0.1;mem:32;disk:32").get();

  TaskInfo task1 =
    createTask(slaveId, resources, "default-command1");

  TaskInfo task2 =
    createTask(slaveId, resources, "default-command2");

  // Compare task groups with identical tasks.
  {
    TaskGroupInfo taskGroup1;
    taskGroup1.add_tasks()->CopyFrom(task1);
    taskGroup1.add_tasks()->CopyFrom(task2);

    TaskGroupInfo taskGroup2;
    taskGroup2.add_tasks()->CopyFrom(task1);
    taskGroup2.add_tasks()->CopyFrom(task2);

    EXPECT_EQ(taskGroup1, taskGroup2);
  }

  // Compare task groups with identical tasks but ordered differently.
  {
    TaskGroupInfo taskGroup1;
    taskGroup1.add_tasks()->CopyFrom(task1);
    taskGroup1.add_tasks()->CopyFrom(task2);

    TaskGroupInfo taskGroup2;
    taskGroup2.add_tasks()->CopyFrom(task2);
    taskGroup2.add_tasks()->CopyFrom(task1);

    EXPECT_EQ(taskGroup1, taskGroup2);
  }

  // Compare task groups with unequal tasks.
  {
    TaskGroupInfo taskGroup1;
    taskGroup1.add_tasks()->CopyFrom(task1);
    taskGroup1.add_tasks()->CopyFrom(task2);

    TaskGroupInfo taskGroup2;
    taskGroup2.add_tasks()->CopyFrom(task1);

    EXPECT_FALSE(taskGroup1 == taskGroup2);
  }
}
