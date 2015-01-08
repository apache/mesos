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

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

using std::string;
using strings::format;

class PathsTest : public ::testing::Test
{
public:
  PathsTest()
  {
    slaveId.set_value("slave1");
    frameworkId.set_value("framework1");
    executorId.set_value("executor1");
    taskId.set_value("task1");
    containerId.set_value(UUID::random().toString());
    role = "role1";
    persistenceId = "persistenceId1";

    Try<string> path = os::mkdtemp();
    CHECK_SOME(path) << "Failed to mkdtemp";
    rootDir = path.get();
  }

  virtual ~PathsTest()
  {
     os::rmdir(rootDir);
  }

protected:
  SlaveID slaveId;
  FrameworkID frameworkId;
  ExecutorID executorId;
  TaskID taskId;
  ContainerID containerId;
  string role;
  string persistenceId;
  string rootDir;
};


TEST_F(PathsTest, CreateExecutorDirectory)
{
  const string& result = paths::createExecutorDirectory(
      rootDir, slaveId, frameworkId, executorId, containerId);

  // Expected directory layout.
  string dir = path::join(
      rootDir,
      "slaves",
      slaveId.value(),
      "frameworks",
      frameworkId.value(),
      "executors",
      executorId.value(),
      "runs",
      containerId.value());

  ASSERT_EQ(dir, result);
}


TEST_F(PathsTest, Format)
{
  string dir = rootDir;

  dir = path::join(dir, "slaves", slaveId.value());

  EXPECT_EQ(dir, paths::getSlavePath(rootDir, slaveId));

  dir = path::join(dir, "frameworks", frameworkId.value());

  EXPECT_EQ(dir, paths::getFrameworkPath(rootDir, slaveId, frameworkId));

  dir = path::join(dir, "executors", executorId.value());

  EXPECT_EQ(dir, paths::getExecutorPath(
      rootDir, slaveId, frameworkId, executorId));

  dir = path::join(dir, "runs", containerId.value());

  EXPECT_EQ(dir, paths::getExecutorRunPath(
      rootDir, slaveId, frameworkId, executorId, containerId));

  dir = path::join(dir, "tasks", taskId.value());

  EXPECT_EQ(dir, paths::getTaskPath(
      rootDir, slaveId, frameworkId, executorId, containerId, taskId));
}


TEST_F(PathsTest, PersistentVolume)
{
  string dir = path::join(rootDir, "volumes", "roles", role, persistenceId);

  EXPECT_EQ(dir, paths::getPersistentVolumePath(rootDir, role, persistenceId));
}

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
