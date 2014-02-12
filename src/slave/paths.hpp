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

#ifndef __SLAVE_PATHS_HPP__
#define __SLAVE_PATHS_HPP__

#include <list>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

const std::string LATEST_SYMLINK = "latest";

// Helper functions to generate paths.

// File names.
const std::string BOOT_ID_FILE = "boot_id";
const std::string SLAVE_INFO_FILE = "slave.info";
const std::string FRAMEWORK_PID_FILE = "framework.pid";
const std::string FRAMEWORK_INFO_FILE = "framework.info";
const std::string LIBPROCESS_PID_FILE = "libprocess.pid";
const std::string EXECUTOR_INFO_FILE = "executor.info";
const std::string EXECUTOR_SENTINEL_FILE = "executor.sentinel";
const std::string FORKED_PID_FILE = "forked.pid";
const std::string TASK_INFO_FILE = "task.info";
const std::string TASK_UPDATES_FILE = "task.updates";

// Path layout templates.
const std::string ROOT_PATH = "%s";
const std::string LATEST_SLAVE_PATH =
  path::join(ROOT_PATH, "slaves", LATEST_SYMLINK);
const std::string SLAVE_PATH =
  path::join(ROOT_PATH, "slaves", "%s");
const std::string BOOT_ID_PATH =
  path::join(ROOT_PATH, BOOT_ID_FILE);
const std::string SLAVE_INFO_PATH =
  path::join(SLAVE_PATH, SLAVE_INFO_FILE);
const std::string FRAMEWORK_PATH =
  path::join(SLAVE_PATH, "frameworks", "%s");
const std::string FRAMEWORK_PID_PATH =
  path::join(FRAMEWORK_PATH, FRAMEWORK_PID_FILE);
const std::string FRAMEWORK_INFO_PATH =
  path::join(FRAMEWORK_PATH, FRAMEWORK_INFO_FILE);
const std::string EXECUTOR_PATH =
  path::join(FRAMEWORK_PATH, "executors", "%s");
const std::string EXECUTOR_INFO_PATH =
  path::join(EXECUTOR_PATH, EXECUTOR_INFO_FILE);
const std::string EXECUTOR_RUN_PATH =
  path::join(EXECUTOR_PATH, "runs", "%s");
const std::string EXECUTOR_SENTINEL_PATH =
  path::join(EXECUTOR_RUN_PATH, EXECUTOR_SENTINEL_FILE);
const std::string EXECUTOR_LATEST_RUN_PATH =
  path::join(EXECUTOR_PATH, "runs", LATEST_SYMLINK);
const std::string PIDS_PATH =
  path::join(EXECUTOR_RUN_PATH, "pids");
const std::string LIBPROCESS_PID_PATH =
  path::join(PIDS_PATH, LIBPROCESS_PID_FILE);
const std::string FORKED_PID_PATH =
  path::join(PIDS_PATH, FORKED_PID_FILE);
const std::string TASK_PATH =
  path::join(EXECUTOR_RUN_PATH, "tasks", "%s");
const std::string TASK_INFO_PATH =
  path::join(TASK_PATH, TASK_INFO_FILE);
const std::string TASK_UPDATES_PATH =
  path::join(TASK_PATH, TASK_UPDATES_FILE);


inline std::string getMetaRootDir(const std::string rootDir)
{
  return path::join(rootDir, "meta");
}


inline std::string getArchiveDir(const std::string rootDir)
{
  return path::join(rootDir, "archive");
}


inline std::string getLatestSlavePath(const std::string& rootDir)
{
  return strings::format(LATEST_SLAVE_PATH, rootDir).get();
}


inline std::string getBootIdPath(const std::string& rootDir)
{
  return strings::format(BOOT_ID_PATH, rootDir).get();
}


inline std::string getSlaveInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId)
{
  return strings::format(SLAVE_INFO_PATH, rootDir, slaveId).get();
}


inline std::string getSlavePath(
    const std::string& rootDir,
    const SlaveID& slaveId)
{
  return strings::format(SLAVE_PATH, rootDir, slaveId).get();
}


inline std::string getFrameworkPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_PATH, rootDir, slaveId, frameworkId).get();
}


inline std::string getFrameworkPidPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_PID_PATH, rootDir, slaveId, frameworkId).get();
}


inline std::string getFrameworkInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_INFO_PATH, rootDir, slaveId, frameworkId).get();
}


inline std::string getExecutorPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return strings::format(
      EXECUTOR_PATH, rootDir, slaveId, frameworkId, executorId).get();
}


inline std::string getExecutorInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return strings::format(
      EXECUTOR_INFO_PATH, rootDir, slaveId, frameworkId, executorId).get();
}


inline std::string getExecutorRunPath(
    const std::string& rootDir,
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


inline std::string getExecutorSentinelPath(
    const std::string& rootDir,
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


inline std::string getExecutorLatestRunPath(
    const std::string& rootDir,
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


inline std::string getLibprocessPidPath(
    const std::string& rootDir,
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


inline std::string getForkedPidPath(
    const std::string& rootDir,
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


inline std::string getTaskPath(
    const std::string& rootDir,
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


inline std::string getTaskInfoPath(
    const std::string& rootDir,
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


inline std::string getTaskUpdatesPath(
    const std::string& rootDir,
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


inline std::string createExecutorDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  std::string directory =
    getExecutorRunPath(rootDir, slaveId, frameworkId, executorId, containerId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create executor directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  std::string latest =
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


inline std::string createSlaveDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId)
{
  std::string directory = getSlavePath(rootDir, slaveId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create slave directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  std::string latest = getLatestSlavePath(rootDir);

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

#endif // __SLAVE_PATHS_HPP__
