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

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "tests/mesos.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

using std::string;
using strings::format;

class PathsTest : public ::testing::Test
{
public:
  void SetUp() override
  {
    slaveId.set_value("agent1");
    frameworkId.set_value("framework1");
    executorId.set_value("executor1");
    taskId.set_value("task1");
    containerId.set_value(id::UUID::random().toString());
    role = "role1";
    persistenceId = "persistenceId1";

    Try<string> path = os::mkdtemp();
    ASSERT_SOME(path) << "Failed to mkdtemp";
    rootDir = path.get();

    path = os::mkdtemp();
    ASSERT_SOME(path) << "Failed to mkdtemp";
    diskSourceDir = path.get();

    imageType = Image::APPC;
  }

  void TearDown() override
  {
     os::rmdir(rootDir);
     os::rmdir(diskSourceDir);
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
  string diskSourceDir;
  Image::Type imageType;
};


TEST_F(PathsTest, CreateExecutorDirectory)
{
  Try<string> result = paths::createExecutorDirectory(
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

  ASSERT_SOME_EQ(dir, result);
}


TEST_F(PathsTest, ParseExecutorRunPath)
{
  string goodDir = paths::getExecutorRunPath(
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId);

  string badDir1 = paths::getExecutorPath(
      "/some/other/root",
      slaveId,
      frameworkId,
      executorId);

  string badDir2 = paths::getExecutorPath(
      rootDir,
      slaveId,
      frameworkId,
      executorId);

  string badDir3 = path::join(
      rootDir,
      "notslaves",
      slaveId.value(),
      "notframeworks",
      frameworkId.value(),
      "notexecutors",
      executorId.value(),
      "notruns",
      containerId.value());

  Try<ExecutorRunPath> path = paths::parseExecutorRunPath(rootDir, goodDir);
  ASSERT_SOME(path);

  EXPECT_EQ(slaveId, path->slaveId);
  EXPECT_EQ(frameworkId, path->frameworkId);
  EXPECT_EQ(executorId, path->executorId);
  EXPECT_EQ(containerId, path->containerId);

  path = paths::parseExecutorRunPath(rootDir, badDir1);
  ASSERT_ERROR(path);

  path = paths::parseExecutorRunPath(rootDir, badDir2);
  ASSERT_ERROR(path);

  path = paths::parseExecutorRunPath(rootDir, badDir3);
  ASSERT_ERROR(path);
}


TEST_F(PathsTest, Meta)
{
  EXPECT_EQ(path::join(rootDir, "meta"), paths::getMetaRootDir(rootDir));
}


TEST_F(PathsTest, ProvisionerDir)
{
  EXPECT_EQ(path::join(rootDir, "provisioner"),
            paths::getProvisionerDir(rootDir));
}


TEST_F(PathsTest, BootId)
{
  EXPECT_EQ(path::join(rootDir, "boot_id"), paths::getBootIdPath(rootDir));
}


TEST_F(PathsTest, Slave)
{
  const string slavesRoot = path::join(rootDir, "slaves");

  EXPECT_EQ(path::join(slavesRoot, "latest"),
            paths::getLatestSlavePath(rootDir));

  EXPECT_EQ(path::join(slavesRoot, slaveId.value()),
            paths::getSlavePath(rootDir, slaveId));
}


TEST_F(PathsTest, Framework)
{
  const string frameworksRoot =
      path::join(paths::getSlavePath(rootDir, slaveId), "frameworks");

  EXPECT_EQ(path::join(frameworksRoot, frameworkId.value()),
            paths::getFrameworkPath(rootDir, slaveId, frameworkId));
}


TEST_F(PathsTest, Executor)
{
  const string executorsRoot =
      path::join(
          paths::getFrameworkPath(
              rootDir,
              slaveId,
              frameworkId),
          "executors");

  EXPECT_EQ(path::join(executorsRoot, executorId.value()),
            paths::getExecutorPath(rootDir, slaveId, frameworkId, executorId));

  EXPECT_EQ(
      path::join(
          executorsRoot,
          executorId.value(),
          "runs",
          containerId.value()),
      paths::getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId));
}


TEST_F(PathsTest, LibProcessPid)
{
  EXPECT_EQ(
      path::join(
          getExecutorRunPath(
              rootDir,
              slaveId,
              frameworkId,
              executorId,
              containerId),
          "pids",
          "libprocess.pid"),
      paths::getLibprocessPidPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId));
}


TEST_F(PathsTest, Task)
{
  const string tasksRoot =
      path::join(
          paths::getExecutorRunPath(
              rootDir,
              slaveId,
              frameworkId,
              executorId,
              containerId),
          "tasks");

  EXPECT_EQ(
      path::join(tasksRoot, taskId.value()),
      paths::getTaskPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId,
          taskId));
}


TEST_F(PathsTest, PersistentVolume)
{
  string dir = path::join(rootDir, "volumes", "roles", role, persistenceId);

  EXPECT_EQ(dir, paths::getPersistentVolumePath(rootDir, role, persistenceId));

  dir = path::join(diskSourceDir, "volumes", "roles", role, persistenceId);

  Resource disk = tests::createPersistentVolume(
      Megabytes(1024),
      "role1",
      persistenceId,
      "path1",
      None());

  disk.mutable_disk()->mutable_source()->set_type(
      Resource::DiskInfo::Source::PATH);

  disk.mutable_disk()->mutable_source()->mutable_path()->set_root(
      diskSourceDir);

  EXPECT_EQ(
      dir,
      paths::getPersistentVolumePath(rootDir, disk));

  disk.mutable_disk()->mutable_source()->set_type(
      Resource::DiskInfo::Source::MOUNT);

  disk.mutable_disk()->mutable_source()->clear_path();
  disk.mutable_disk()->mutable_source()->mutable_mount()->set_root(
      diskSourceDir);

  EXPECT_EQ(
      diskSourceDir,
      paths::getPersistentVolumePath(rootDir, disk));
}


} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
