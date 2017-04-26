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

#include <list>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/roles.hpp>
#include <mesos/type_utils.hpp>

#include <stout/check.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <glog/logging.h>

#include "common/validation.hpp"

#include "messages/messages.hpp"

#include "slave/paths.hpp"
#include "slave/validation.hpp"

using std::list;
using std::string;
using std::vector;

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
const char HTTP_MARKER_FILE[] = "http.marker";
const char FORKED_PID_FILE[] = "forked.pid";
const char TASK_INFO_FILE[] = "task.info";
const char TASK_UPDATES_FILE[] = "task.updates";
const char RESOURCES_INFO_FILE[] = "resources.info";
const char RESOURCES_TARGET_FILE[] = "resources.target";


const char SLAVES_DIR[] = "slaves";
const char FRAMEWORKS_DIR[] = "frameworks";
const char EXECUTORS_DIR[] = "executors";
const char CONTAINERS_DIR[] = "runs";


Try<ExecutorRunPath> parseExecutorRunPath(
    const string& _rootDir,
    const string& dir)
{
  // TODO(josephw): Consider using `<regex>` here, which requires GCC 4.9+.

  // Make sure there's a separator at the end of the `rootdir` so that
  // we don't accidentally slice off part of a directory.
  const string rootDir = path::join(_rootDir, "");

  if (!strings::startsWith(dir, rootDir)) {
    return Error(
        "Directory '" + dir + "' does not fall under "
        "the root directory: " + rootDir);
  }

  vector<string> tokens = strings::tokenize(dir.substr(rootDir.size()), "/");

  // A complete executor run path consists of at least 8 tokens, which
  // includes the four named directories and the four IDs.
  if (tokens.size() < 8) {
    return Error(
        "Path after root directory is not long enough to be an "
        "executor run path: " + path::join(tokens));
  }

  // All four named directories much match.
  if (tokens[0] == SLAVES_DIR &&
      tokens[2] == FRAMEWORKS_DIR &&
      tokens[4] == EXECUTORS_DIR &&
      tokens[6] == CONTAINERS_DIR) {
    ExecutorRunPath path;

    path.slaveId.set_value(tokens[1]);
    path.frameworkId.set_value(tokens[3]);
    path.executorId.set_value(tokens[5]);
    path.containerId.set_value(tokens[7]);

    return path;
  }

  return Error("Could not parse executor run path from directory: " + dir);
}


string getMetaRootDir(const string& rootDir)
{
  return path::join(rootDir, "meta");
}


string getSandboxRootDir(const string& rootDir)
{
  return path::join(rootDir, SLAVES_DIR);
}


string getProvisionerDir(const string& rootDir)
{
  return path::join(rootDir, "provisioner");
}


string getBootIdPath(const string& rootDir)
{
  return path::join(rootDir, BOOT_ID_FILE);
}


string getLatestSlavePath(const string& rootDir)
{
  return path::join(rootDir, SLAVES_DIR, LATEST_SYMLINK);
}


string getSlavePath(
    const string& rootDir,
    const SlaveID& slaveId)
{
  return path::join(rootDir, SLAVES_DIR, stringify(slaveId));
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
  return fs::list(
      path::join(getSlavePath(rootDir, slaveId), FRAMEWORKS_DIR, "*"));
}


string getFrameworkPath(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId)
{
  return path::join(
      getSlavePath(rootDir, slaveId), FRAMEWORKS_DIR, stringify(frameworkId));
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
  return fs::list(path::join(
      getFrameworkPath(rootDir, slaveId, frameworkId),
      EXECUTORS_DIR,
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
        EXECUTORS_DIR,
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
  return fs::list(path::join(
      getExecutorPath(rootDir, slaveId, frameworkId, executorId),
      CONTAINERS_DIR,
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
      CONTAINERS_DIR,
      stringify(containerId));
}


string getExecutorHttpMarkerPath(
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
          HTTP_MARKER_FILE);
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
      CONTAINERS_DIR,
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
  return fs::list(path::join(
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


string getResourcesTargetPath(
    const string& rootDir)
{
  return path::join(rootDir, "resources", RESOURCES_TARGET_FILE);
}


string getPersistentVolumePath(
    const string& rootDir,
    const string& role,
    const string& persistenceId)
{
  // Role names might contain literal `/` if the role is part of a
  // role hierarchy. Since `/` is not allowed in a directory name
  // under Linux, we could either represent such sub-roles with
  // sub-directories, or encode the `/` with some other identifier.
  // To clearly distinguish artifacts in a volume from subroles we
  // choose to encode `/` in role names as ` ` (literal space) as
  // opposed to using subdirectories. Whitespace is not allowed as
  // part of a role name. Also, practically all modern filesystems can
  // use ` ` in filenames. There are some limitations in auxilary
  // tooling which are not relevant here, e.g., many shell constructs
  // require quotes around filesnames containing ` `; containers using
  // persistent volumes would not see the ` ` as the role-related part
  // of the path would not be part of a mapping into the container
  // sandbox.
  string serializableRole = strings::replace(role, "/", " ");

  return path::join(
      rootDir, "volumes", "roles", serializableRole, persistenceId);
}


string getPersistentVolumePath(
    const string& rootDir,
    const Resource& volume)
{
  CHECK(volume.has_role());
  CHECK(volume.has_disk());
  CHECK(volume.disk().has_persistence());

  // Additionally check that the role and the persistent ID are valid
  // before using them to construct a directory path.
  CHECK_NONE(roles::validate(volume.role()));
  CHECK_NONE(common::validation::validateID(volume.disk().persistence().id()));

  // If no `source` is provided in `DiskInfo` volumes are mapped into
  // the `rootDir`.
  if (!volume.disk().has_source()) {
    return getPersistentVolumePath(
        rootDir,
        volume.role(),
        volume.disk().persistence().id());
  }

  // If a `source` was provided for the volume, we map it according
  // to the `type` of disk. Currently only the `PATH` and 'MOUNT'
  // types are supported.
  switch (volume.disk().source().type()) {
    case Resource::DiskInfo::Source::PATH: {
      // For `PATH` we mount a directory inside the `root`.
      CHECK(volume.disk().source().has_path());
      CHECK(volume.disk().source().path().has_root());
      return getPersistentVolumePath(
          volume.disk().source().path().root(),
          volume.role(),
          volume.disk().persistence().id());
    }
    case Resource::DiskInfo::Source::MOUNT: {
      // For `MOUNT` we map straight onto the root of the mount.
      CHECK(volume.disk().source().has_mount());
      CHECK(volume.disk().source().mount().has_root());
      return volume.disk().source().mount().root();
    }
    case Resource::DiskInfo::Source::UNKNOWN:
      LOG(FATAL) << "Unsupported DiskInfo.Source.type";
      break;
  }

  UNREACHABLE();
}


string createExecutorDirectory(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Option<string>& user)
{
  // These IDs should be valid as they are either assigned by the
  // master/agent or validated by the master but we do a sanity check
  // here before using them to create a directory.
  CHECK_NONE(common::validation::validateSlaveID(slaveId));
  CHECK_NONE(common::validation::validateFrameworkID(frameworkId));
  CHECK_NONE(common::validation::validateExecutorID(executorId));
  CHECK_NONE(slave::validation::container::validateContainerId(containerId));

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

// `os::chown()` is not available on Windows.
#ifndef __WINDOWS__
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
#endif // __WINDOWS__

  return directory;
}


string createSlaveDirectory(
    const string& rootDir,
    const SlaveID& slaveId)
{
  // `slaveId` should be valid because it's assigned by the master but
  // we do a sanity check here before using it to create a directory.
  CHECK_NONE(common::validation::validateSlaveID(slaveId));

  const string directory = getSlavePath(rootDir, slaveId);

  Try<Nothing> mkdir = os::mkdir(directory);

  CHECK_SOME(mkdir)
    << "Failed to create agent directory '" << directory << "'";

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
