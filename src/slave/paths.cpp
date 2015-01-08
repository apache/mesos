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

#include <list>
#include <string>

#include <mesos/mesos.hpp>

#include <stout/check.hpp>
#include <stout/format.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "slave/paths.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

// TODO(bmahler): Replace these non-POD globals, per MESOS-1023.

// File names.
const string BOOT_ID_FILE = "boot_id";
const string SLAVE_INFO_FILE = "slave.info";
const string FRAMEWORK_PID_FILE = "framework.pid";
const string FRAMEWORK_INFO_FILE = "framework.info";
const string LIBPROCESS_PID_FILE = "libprocess.pid";
const string EXECUTOR_INFO_FILE = "executor.info";
const string EXECUTOR_SENTINEL_FILE = "executor.sentinel";
const string FORKED_PID_FILE = "forked.pid";
const string TASK_INFO_FILE = "task.info";
const string TASK_UPDATES_FILE = "task.updates";
const string RESOURCES_INFO_FILE = "resources.info";

// Path layout templates.
const string ROOT_PATH = "%s";
const string LATEST_SLAVE_PATH =
  path::join(ROOT_PATH, "slaves", LATEST_SYMLINK);
const string SLAVE_PATH =
  path::join(ROOT_PATH, "slaves", "%s");
const string BOOT_ID_PATH =
  path::join(ROOT_PATH, BOOT_ID_FILE);
const string SLAVE_INFO_PATH =
  path::join(SLAVE_PATH, SLAVE_INFO_FILE);
const string FRAMEWORK_PATH =
  path::join(SLAVE_PATH, "frameworks", "%s");
const string FRAMEWORK_PID_PATH =
  path::join(FRAMEWORK_PATH, FRAMEWORK_PID_FILE);
const string FRAMEWORK_INFO_PATH =
  path::join(FRAMEWORK_PATH, FRAMEWORK_INFO_FILE);
const string EXECUTOR_PATH =
  path::join(FRAMEWORK_PATH, "executors", "%s");
const string EXECUTOR_INFO_PATH =
  path::join(EXECUTOR_PATH, EXECUTOR_INFO_FILE);
const string EXECUTOR_RUN_PATH =
  path::join(EXECUTOR_PATH, "runs", "%s");
const string EXECUTOR_SENTINEL_PATH =
  path::join(EXECUTOR_RUN_PATH, EXECUTOR_SENTINEL_FILE);
const string EXECUTOR_LATEST_RUN_PATH =
  path::join(EXECUTOR_PATH, "runs", LATEST_SYMLINK);
const string PIDS_PATH =
  path::join(EXECUTOR_RUN_PATH, "pids");
const string LIBPROCESS_PID_PATH =
  path::join(PIDS_PATH, LIBPROCESS_PID_FILE);
const string FORKED_PID_PATH =
  path::join(PIDS_PATH, FORKED_PID_FILE);
const string TASK_PATH =
  path::join(EXECUTOR_RUN_PATH, "tasks", "%s");
const string TASK_INFO_PATH =
  path::join(TASK_PATH, TASK_INFO_FILE);
const string TASK_UPDATES_PATH =
  path::join(TASK_PATH, TASK_UPDATES_FILE);
const string RESOURCES_INFO_PATH =
  path::join(ROOT_PATH, "resources", RESOURCES_INFO_FILE);
const string PERSISTENT_VOLUME_PATH =
  path::join(ROOT_PATH, "volumes", "roles", "%s", "%s");


string getMetaRootDir(const string& rootDir)
{
  return path::join(rootDir, "meta");
}


string getArchiveDir(const string& rootDir)
{
  return path::join(rootDir, "archive");
}


string getLatestSlavePath(const string& rootDir)
{
  return strings::format(LATEST_SLAVE_PATH, rootDir).get();
}


string getBootIdPath(const string& rootDir)
{
  return strings::format(BOOT_ID_PATH, rootDir).get();
}


string getSlaveInfoPath(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return strings::format(SLAVE_INFO_PATH, rootDir, slaveId).get();
}


string getSlavePath(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return strings::format(SLAVE_PATH, rootDir, slaveId).get();
}


Try<list<string>> getFrameworkPaths(
    const string& rootDir,
    const SlaveID& slaveId)
{
  Try<string> format = strings::format(
      paths::FRAMEWORK_PATH,
      rootDir,
      slaveId,
      "*");

  CHECK_SOME(format);

  return os::glob(format.get());
}


string getFrameworkPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_PATH, rootDir, slaveId, frameworkId).get();
}


string getFrameworkPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_PID_PATH, rootDir, slaveId, frameworkId).get();
}


string getFrameworkInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_INFO_PATH, rootDir, slaveId, frameworkId).get();
}


Try<list<string>> getExecutorPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  Try<string> format = strings::format(
      paths::EXECUTOR_PATH,
      rootDir,
      slaveId,
      frameworkId,
      "*");

  CHECK_SOME(format);

  return os::glob(format.get());
}


string getExecutorPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return strings::format(
      EXECUTOR_PATH, rootDir, slaveId, frameworkId, executorId).get();
}


string getExecutorInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return strings::format(
      EXECUTOR_INFO_PATH, rootDir, slaveId, frameworkId, executorId).get();
}


Try<list<string>> getExecutorRunPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  Try<string> format = strings::format(
      paths::EXECUTOR_RUN_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      "*");

  CHECK_SOME(format);

  return os::glob(format.get());
}


string getExecutorRunPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return strings::format(
      EXECUTOR_RUN_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId).get();
}


string getExecutorSentinelPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return strings::format(
      EXECUTOR_SENTINEL_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId).get();
}


string getExecutorLatestRunPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return strings::format(
      EXECUTOR_LATEST_RUN_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId).get();
}


string getLibprocessPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return strings::format(
      LIBPROCESS_PID_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId).get();
}


string getForkedPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return strings::format(
      FORKED_PID_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId).get();
}


Try<list<string>> getTaskPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Try<string> format = strings::format(
      paths::TASK_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId,
      "*");

  CHECK_SOME(format);

  return os::glob(format.get());
}


string getTaskPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return strings::format(
      TASK_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId,
      taskId).get();
}


string getTaskInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return strings::format(
      TASK_INFO_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId,
      taskId).get();
}


string getTaskUpdatesPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return strings::format(
      TASK_UPDATES_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId,
      taskId).get();
}


string getResourcesInfoPath(
    const string& rootDir)
{
  return strings::format(
      RESOURCES_INFO_PATH,
      rootDir).get();
}


string getPersistentVolumePath(
    const string& rootDir,
    const string& role,
    const string& persistenceId)
{
  return strings::format(
      PERSISTENT_VOLUME_PATH,
      rootDir,
      role,
      persistenceId).get();
}


string createExecutorDirectory(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  string directory =
    getExecutorRunPath(rootDir, slaveId, frameworkId, executorId, containerId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create executor directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  string latest =
    getExecutorLatestRunPath(rootDir, slaveId, frameworkId, executorId);

  if (os::exists(latest)) {
    CHECK_SOME(os::rm(latest))
      << "Failed to remove latest symlink '" << latest << "'";
  }

  // Symlink the new executor directory to "latest".
  Try<Nothing> symlink = ::fs::symlink(directory, latest);

  CHECK_SOME(symlink)
    << "Failed to symlink directory '" << directory
    << "' to '" << latest << "'";

  return directory;
}


string createSlaveDirectory(
    const string& rootDir,
    const SlaveID& slaveId)
{
  string directory = getSlavePath(rootDir, slaveId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create slave directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  string latest = getLatestSlavePath(rootDir);

  if (os::exists(latest)) {
    CHECK_SOME(os::rm(latest))
      << "Failed to remove latest symlink '" << latest << "'";
  }

  // Symlink the new slave directory to "latest".
  Try<Nothing> symlink = ::fs::symlink(directory, latest);

  CHECK_SOME(symlink)
    << "Failed to symlink directory '" << directory
    << "' to '" << latest << "'";

  return directory;
}

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
