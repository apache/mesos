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

#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

const std::string EXECUTOR_LATEST_SYMLINK = "latest";

// Helper functions to generate paths.

// File names.
const std::string SLAVEID_FILE = "slave.id";
const std::string FRAMEWORK_PID_FILE = "framework.pid";
const std::string LIBPROCESS_PID_FILE = "libprocess.pid";
const std::string FORKED_PID_FILE = "forked.pid";
const std::string TASK_INFO_FILE = "info";
const std::string TASK_UPDATES_FILE = "updates";

// Path layout templates.
const std::string ROOT_PATH = "%s";
const std::string SLAVEID_PATH = ROOT_PATH + "/slaves/" + SLAVEID_FILE;
const std::string SLAVE_PATH = ROOT_PATH + "/slaves/%s";
const std::string FRAMEWORK_PATH = SLAVE_PATH + "/frameworks/%s";
const std::string FRAMEWORK_PID_PATH =
    FRAMEWORK_PATH + "/" + FRAMEWORK_PID_FILE;
const std::string EXECUTOR_PATH = FRAMEWORK_PATH + "/executors/%s";
const std::string EXECUTOR_RUN_PATH = EXECUTOR_PATH + "/runs/%s";
const std::string EXECUTOR_LATEST_RUN_PATH =
  EXECUTOR_PATH + "/runs/" + EXECUTOR_LATEST_SYMLINK;
const std::string PIDS_PATH = EXECUTOR_RUN_PATH + "/pids";
const std::string LIBPROCESS_PID_PATH = PIDS_PATH + "/" + LIBPROCESS_PID_FILE;
const std::string FORKED_PID_PATH = PIDS_PATH + "/" + FORKED_PID_FILE;
const std::string TASK_PATH = EXECUTOR_RUN_PATH + "/tasks/%s";
const std::string TASK_INFO_PATH = TASK_PATH + "/" + TASK_INFO_FILE;
const std::string TASK_UPDATES_PATH = TASK_PATH + "/" + TASK_UPDATES_FILE;


inline std::string getMetaRootDir(const std::string rootDir)
{
  return path::join(rootDir, "meta");
}


inline std::string getSlaveIDPath(const std::string& rootDir)
{
  return strings::format(SLAVEID_PATH, rootDir).get();
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


inline std::string getFrameworkPIDPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return strings::format(
      FRAMEWORK_PID_PATH, rootDir, slaveId, frameworkId).get();
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


inline std::string getExecutorRunPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID)
{
  return strings::format(
      EXECUTOR_RUN_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString()).get();
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


inline std::string getLibprocessPIDPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID)
{
  return strings::format(
      LIBPROCESS_PID_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString()).get();
}


inline std::string getForkedPIDPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID)
{
  return strings::format(
      FORKED_PID_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString()).get();
}


inline std::string getTaskPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID,
    const TaskID& taskId)
{
  return strings::format(
      TASK_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString(),
      taskId).get();
}


inline std::string getTaskInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID,
    const TaskID& taskId)
{
  return strings::format(
      TASK_INFO_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString(),
      taskId).get();
}


inline std::string getTaskUpdatesPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID,
    const TaskID& taskId)
{
  return strings::format(
      TASK_UPDATES_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      executorUUID.toString(),
      taskId).get();
}


inline std::string createExecutorDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& executorUUID)
{
  std::string directory =
    getExecutorRunPath(rootDir, slaveId, frameworkId, executorId, executorUUID);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create executor directory '" << directory << "'";
  LOG(INFO) << "Created executor directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  std::string latest =
    getExecutorLatestRunPath(rootDir, slaveId, frameworkId, executorId);

  if (os::exists(latest)) {
    CHECK_SOME(os::rm(latest)) << "Failed to remove latest symlink " << latest;
  }

  // Symlink the new executor directory to "latest".
  Try<Nothing> symlink = fs::symlink(directory, latest);

  CHECK_SOME(symlink)
    << "Failed to symlink latest work directory '" << directory << "'";

  return directory;
}

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_PATHS_HPP__
