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

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "tests/utils.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

using std::string;
using strings::format;

class SlaveStateFixture: public ::testing::Test
{
protected:
  SlaveStateFixture()
    : uuid(UUID::random())
  {
    slaveId.set_value("slave1");
    frameworkId.set_value("framework1");
    executorId.set_value("executor1");
    taskId.set_value("task1");

    Try<string> path = os::mkdtemp();
    CHECK_SOME(path) << "Failed to mkdtemp";
    rootDir = path.get();
  }

  virtual ~SlaveStateFixture()
  {
     os::rmdir(rootDir);
  }

  SlaveID slaveId;
  FrameworkID frameworkId;
  ExecutorID executorId;
  TaskID taskId;
  UUID uuid;
  string rootDir;
};


TEST_F(SlaveStateFixture, CreateExecutorDirectory)
{
  const string& result = paths::createExecutorDirectory(
      rootDir, slaveId, frameworkId, executorId, uuid);

  // Expected directory layout.
  string dir = rootDir + "/slaves/" + slaveId.value() + "/frameworks/"
               + frameworkId.value() + "/executors/" + executorId.value()
               + "/runs/" + uuid.toString();

  ASSERT_EQ(dir, result);
}


TEST_F(SlaveStateFixture, format)
{
  string dir = rootDir;

  dir += "/slaves/" + slaveId.value();
  ASSERT_EQ(dir, paths::getSlavePath(rootDir, slaveId));

  dir += "/frameworks/" + frameworkId.value();
  ASSERT_EQ(dir, paths::getFrameworkPath(rootDir, slaveId, frameworkId));

  dir += "/executors/" + executorId.value();
  ASSERT_EQ(dir, paths::getExecutorPath(rootDir, slaveId, frameworkId,
                                        executorId));

  dir += "/runs/" + uuid.toString();
  ASSERT_EQ(dir, paths::getExecutorRunPath(rootDir, slaveId, frameworkId,
                                           executorId, uuid));

  dir += "/tasks/" + taskId.value();
  ASSERT_EQ(dir, paths::getTaskPath(rootDir, slaveId, frameworkId, executorId,
                                    uuid, taskId));
}


TEST_F(SlaveStateFixture, parse)
{
  // Create some layouts and check if parse works as expected.
  const string& executorDir = paths::createExecutorDirectory(
      rootDir, slaveId, frameworkId, executorId, uuid);

  // Write framework pid file.
  const string& frameworkpidPath = paths::getFrameworkPIDPath(rootDir, slaveId,
                                                              frameworkId);
  ASSERT_SOME(os::touch(frameworkpidPath));

  // Write process pid files.
  ASSERT_SOME(os::mkdir(executorDir + "/pids"));

  const string& libpidPath = paths::getLibprocessPIDPath(rootDir, slaveId,
                                                         frameworkId, executorId,
                                                         uuid);

  const string& forkedpidPath = paths::getForkedPIDPath(rootDir, slaveId,
                                                        frameworkId, executorId,
                                                        uuid);

  const string& execedpidPath = paths::getExecedPIDPath(rootDir, slaveId,
                                                        frameworkId, executorId,
                                                        uuid);

  ASSERT_SOME(os::touch(libpidPath));
  ASSERT_SOME(os::touch(forkedpidPath));
  ASSERT_SOME(os::touch(execedpidPath));

  // Write task and updates files.
  const string& taskDir = paths::getTaskPath(rootDir, slaveId, frameworkId,
                                             executorId, uuid, taskId);

  ASSERT_SOME(os::mkdir(taskDir));

  const string& infoPath = paths::getTaskInfoPath(rootDir, slaveId, frameworkId,
                                                  executorId, uuid, taskId);

  const string& updatesPath = paths::getTaskUpdatesPath(rootDir, slaveId,
                                                        frameworkId, executorId,
                                                        uuid, taskId);

  ASSERT_SOME(os::touch(infoPath));
  ASSERT_SOME(os::touch(updatesPath));

  SlaveState state = parse(rootDir, slaveId);

  ASSERT_TRUE(state.frameworks.contains(frameworkId));
  ASSERT_TRUE(state.frameworks[frameworkId].executors.contains(executorId));
  ASSERT_TRUE(
      state
      .frameworks[frameworkId]
      .executors[executorId]
      .runs
      .contains(uuid));
   ASSERT_TRUE(
       state
       .frameworks[frameworkId]
       .executors[executorId]
       .runs[uuid]
       .tasks
       .contains(taskId));
}


TEST_F(SlaveStateFixture, CheckpointSlaveID)
{
  writeSlaveID(rootDir, slaveId);

  ASSERT_EQ(slaveId, readSlaveID(rootDir));
}


TEST_F(SlaveStateFixture, CheckpointFrameworkPID)
{
  process::UPID upid("random");
  writeFrameworkPID(rootDir, slaveId, frameworkId, upid);

  ASSERT_EQ(upid, readFrameworkPID(rootDir, slaveId, frameworkId));
}

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
