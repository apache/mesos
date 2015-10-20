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
#include <mesos/type_utils.hpp>


#include <stout/check.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

#include "slave/paths.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

// File names.
const char BOOT_ID_FILE[] = "boot_id";
const char SLAVE_INFO_FILE[] = "slave.info";
const char FRAMEWORK_PID_FILE[] = "framework.pid";
const char FRAMEWORK_INFO_FILE[] = "framework.info";
const char LIBPROCESS_PID_FILE[] = "libprocess.pid";
const char EXECUTOR_INFO_FILE[] = "executor.info";
const char EXECUTOR_SENTINEL_FILE[] = "executor.sentinel";
const char FORKED_PID_FILE[] = "forked.pid";
const char TASK_INFO_FILE[] = "task.info";
const char TASK_UPDATES_FILE[] = "task.updates";
const char RESOURCES_INFO_FILE[] = "resources.info";


string getMetaRootDir(const string& rootDir)
{
  return path::join(rootDir, "meta");
}


string getSandboxRootDir(const string& rootDir)
{
  return path::join(rootDir, "slaves");
}


string getProvisionerDir(const string& rootDir)
{
  return path::join(rootDir, "provisioner");
}


string getArchiveDir(const string& rootDir)
{
  return path::join(rootDir, "archive");
}


string getBootIdPath(const string& rootDir)
{
  return path::join(rootDir, BOOT_ID_FILE);
}


string getLatestSlavePath(const string& rootDir)
{
  return path::join(rootDir, "slaves", LATEST_SYMLINK);
}


string getSlavePath(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return path::join(rootDir, "slaves", stringify(slaveId));
}


string getSlaveInfoPath(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return path::join(getSlavePath(rootDir, slaveId), SLAVE_INFO_FILE);
}


Try<list<string>> getFrameworkPaths(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return os::glob(
      path::join(getSlavePath(rootDir, slaveId), "frameworks", "*"));
}


string getFrameworkPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return path::join(
      getSlavePath(rootDir, slaveId), "frameworks", stringify(frameworkId));
}


string getFrameworkPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return path::join(
      getFrameworkPath(rootDir, slaveId, frameworkId), FRAMEWORK_PID_FILE);
}


string getFrameworkInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return path::join(
      getFrameworkPath(rootDir, slaveId, frameworkId), FRAMEWORK_INFO_FILE);
}


Try<list<string>> getExecutorPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return os::glob(path::join(
      getFrameworkPath(rootDir, slaveId, frameworkId),
      "executors",
      "*"));
}


string getExecutorPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return path::join(
        getFrameworkPath(rootDir, slaveId, frameworkId),
        "executors",
        stringify(executorId));
}


string getExecutorInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return path::join(
      getExecutorPath(rootDir, slaveId, frameworkId, executorId),
      EXECUTOR_INFO_FILE);
}


Try<list<string>> getExecutorRunPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return os::glob(path::join(
      getExecutorPath(rootDir, slaveId, frameworkId, executorId),
      "runs",
      "*"));
}


string getExecutorRunPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return path::join(
      getExecutorPath(rootDir, slaveId, frameworkId, executorId),
      "runs",
      stringify(containerId));
}


string getExecutorSentinelPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return path::join(
      getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId),
      EXECUTOR_SENTINEL_FILE);
}


string getExecutorLatestRunPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  return path::join(
      getExecutorPath(rootDir, slaveId, frameworkId, executorId),
      "runs",
      LATEST_SYMLINK);
}


string getLibprocessPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return path::join(
      getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId),
      "pids",
      LIBPROCESS_PID_FILE);
}


string getForkedPidPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return path::join(
      getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId),
      "pids",
      FORKED_PID_FILE);
}


Try<list<string>> getTaskPaths(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return os::glob(path::join(
      getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId),
      "tasks",
      "*"));
}


string getTaskPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return path::join(
      getExecutorRunPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId),
      "tasks",
      stringify(taskId));
}


string getTaskInfoPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return path::join(
      getTaskPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId,
          taskId),
      TASK_INFO_FILE);
}


string getTaskUpdatesPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId)
{
  return path::join(
      getTaskPath(
          rootDir,
          slaveId,
          frameworkId,
          executorId,
          containerId,
          taskId),
      TASK_UPDATES_FILE);
}


string getResourcesInfoPath(
    const string& rootDir)
{
  return path::join(rootDir, "resources", RESOURCES_INFO_FILE);
}


string getPersistentVolumePath(
    const string& rootDir,
    const string& role,
    const string& persistenceId)
{
  return path::join(rootDir, "volumes", "roles", role, persistenceId);
}


string createExecutorDirectory(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Option<string>& user)
{
  const string directory =
    getExecutorRunPath(rootDir, slaveId, frameworkId, executorId, containerId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create executor directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  const string latest =
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

  if (user.isSome()) {
    // Per MESOS-2592, we need to set the ownership of the executor
    // directory during its creation. We should not rely on subsequent
    // phases of the executor creation to ensure the ownership as
    // those may be conditional and in some cases leave the executor
    // directory owned by the slave user instead of the specified
    // framework or per-executor user.
    LOG(INFO) << "Trying to chown '" << directory << "' to user '"
              << user.get() << "'";
    Try<Nothing> chown = os::chown(user.get(), directory);
    if (chown.isError()) {
      // TODO(nnielsen): We currently have tests which depend on using
      // user names which may not be available on the test machines.
      // Therefore, we cannot make the chown validation a hard
      // CHECK().
      LOG(WARNING) << "Failed to chown executor directory '" << directory
                   << "'. This may be due to attempting to run the executor "
                   << "as a nonexistent user on the agent; see the description"
                   << " for the `--switch_user` flag for more information: "
                   << chown.error();
    }
  }

  return directory;
}


string createSlaveDirectory(
    const string& rootDir,
    const SlaveID& slaveId)
{
  const string directory = getSlavePath(rootDir, slaveId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create slave directory '" << directory << "'";

  // Remove the previous "latest" symlink.
  const string latest = getLatestSlavePath(rootDir);

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
