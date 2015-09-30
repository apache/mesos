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
* limitations under the License
*/

#include <unistd.h>

#include <glog/logging.h>

#include <iostream>

#include <process/pid.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/format.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

using std::list;
using std::string;
using std::max;


Result<State> recover(const string& rootDir, bool strict)
{
  LOG(INFO) << "Recovering state from '" << rootDir << "'";

  // We consider the absence of 'rootDir' to mean that this is either
  // the first time this slave was started with checkpointing enabled
  // or this slave was started after an upgrade (--recover=cleanup).
  if (!os::exists(rootDir)) {
    return None();
  }

  // Now, start to recover state from 'rootDir'.
  State state;

  // Recover resources regardless whether the host has rebooted.
  Try<ResourcesState> resources = ResourcesState::recover(rootDir, strict);
  if (resources.isError()) {
    return Error(resources.error());
  }

  // TODO(jieyu): Do not set 'state.resources' if we cannot find the
  // resources checkpoint file.
  state.resources = resources.get();

  // Did the machine reboot? No need to recover slave state if the
  // machine has rebooted.
  if (os::exists(paths::getBootIdPath(rootDir))) {
    Try<string> read = os::read(paths::getBootIdPath(rootDir));
    if (read.isSome()) {
      Try<string> id = os::bootId();
      CHECK_SOME(id);

      if (id.get() != strings::trim(read.get())) {
        LOG(INFO) << "Slave host rebooted";
        return state;
      }
    }
  }

  const std::string& latest = paths::getLatestSlavePath(rootDir);

  // Check if the "latest" symlink to a slave directory exists.
  if (!os::exists(latest)) {
    // The slave was asked to shutdown or died before it registered
    // and had a chance to create the "latest" symlink.
    LOG(INFO) << "Failed to find the latest slave from '" << rootDir << "'";
    return state;
  }

  // Get the latest slave id.
  Result<string> directory = os::realpath(latest);
  if (!directory.isSome()) {
    return Error("Failed to find latest slave: " +
                 (directory.isError()
                  ? directory.error()
                  : "No such file or directory"));
  }

  SlaveID slaveId;
  slaveId.set_value(Path(directory.get()).basename());

  Try<SlaveState> slave = SlaveState::recover(rootDir, slaveId, strict);
  if (slave.isError()) {
    return Error(slave.error());
  }

  state.slave = slave.get();

  return state;
}


Try<SlaveState> SlaveState::recover(
    const string& rootDir,
    const SlaveID& slaveId,
    bool strict)
{
  SlaveState state;
  state.id = slaveId;

  // Read the slave info.
  const string& path = paths::getSlaveInfoPath(rootDir, slaveId);
  if (!os::exists(path)) {
    // This could happen if the slave died before it registered with
    // the master.
    LOG(WARNING) << "Failed to find slave info file '" << path << "'";
    return state;
  }

  const Result<SlaveInfo>& slaveInfo = ::protobuf::read<SlaveInfo>(path);

  if (slaveInfo.isError()) {
    const string& message = "Failed to read slave info from '" + path + "': " +
                            slaveInfo.error();
    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (slaveInfo.isNone()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty slave info file '" << path << "'";
    return state;
  }

  state.info = slaveInfo.get();

  // Find the frameworks.
  Try<list<string> > frameworks = paths::getFrameworkPaths(rootDir, slaveId);

  if (frameworks.isError()) {
    return Error("Failed to find frameworks for slave " + slaveId.value() +
                 ": " + frameworks.error());
  }

  // Recover each of the frameworks.
  foreach (const string& path, frameworks.get()) {
    FrameworkID frameworkId;
    frameworkId.set_value(Path(path).basename());

    Try<FrameworkState> framework =
      FrameworkState::recover(rootDir, slaveId, frameworkId, strict);

    if (framework.isError()) {
      return Error("Failed to recover framework " + frameworkId.value() +
                   ": " + framework.error());
    }

    state.frameworks[frameworkId] = framework.get();
    state.errors += framework.get().errors;
  }

  return state;
}


Try<FrameworkState> FrameworkState::recover(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    bool strict)
{
  FrameworkState state;
  state.id = frameworkId;
  string message;

  // Read the framework info.
  string path = paths::getFrameworkInfoPath(rootDir, slaveId, frameworkId);
  if (!os::exists(path)) {
    // This could happen if the slave died after creating the
    // framework directory but before it checkpointed the framework
    // info.
    LOG(WARNING) << "Failed to find framework info file '" << path << "'";
    return state;
  }

  const Result<FrameworkInfo>& frameworkInfo =
    ::protobuf::read<FrameworkInfo>(path);

  if (frameworkInfo.isError()) {
    message = "Failed to read framework info from '" + path + "': " +
              frameworkInfo.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (frameworkInfo.isNone()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty framework info file '" << path << "'";
    return state;
  }

  state.info = frameworkInfo.get();

  // Read the framework pid.
  path = paths::getFrameworkPidPath(rootDir, slaveId, frameworkId);
  if (!os::exists(path)) {
    // This could happen if the slave died after creating the
    // framework info but before it checkpointed the framework pid.
    LOG(WARNING) << "Failed to framework pid file '" << path << "'";
    return state;
  }

  Try<string> pid = os::read(path);

  if (pid.isError()) {
    message =
      "Failed to read framework pid from '" + path + "': " + pid.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (pid.get().empty()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty framework pid file '" << path << "'";
    return state;
  }

  state.pid = process::UPID(pid.get());

  // Find the executors.
  Try<list<string> > executors =
    paths::getExecutorPaths(rootDir, slaveId, frameworkId);

  if (executors.isError()) {
    return Error(
        "Failed to find executors for framework " + frameworkId.value() +
        ": " + executors.error());
  }

  // Recover the executors.
  foreach (const string& path, executors.get()) {
    ExecutorID executorId;
    executorId.set_value(Path(path).basename());

    Try<ExecutorState> executor =
      ExecutorState::recover(rootDir, slaveId, frameworkId, executorId, strict);

    if (executor.isError()) {
      return Error("Failed to recover executor " + executorId.value() +
                   ": " + executor.error());
    }

    state.executors[executorId] = executor.get();
    state.errors += executor.get().errors;
  }

  return state;
}


Try<ExecutorState> ExecutorState::recover(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    bool strict)
{
  ExecutorState state;
  state.id = executorId;
  string message;

  // Find the runs.
  Try<list<string> > runs = paths::getExecutorRunPaths(
      rootDir,
      slaveId,
      frameworkId,
      executorId);

  if (runs.isError()) {
    return Error("Failed to find runs for executor '" + executorId.value() +
                 "': " + runs.error());
  }

  // Recover the runs.
  foreach (const string& path, runs.get()) {
    if (Path(path).basename() == paths::LATEST_SYMLINK) {
      const Result<string>& latest = os::realpath(path);
      if (!latest.isSome()) {
        return Error(
            "Failed to find latest run of executor '" +
            executorId.value() + "': " +
            (latest.isError()
             ? latest.error()
             : "No such file or directory"));
      }

      // Store the ContainerID of the latest executor run.
      ContainerID containerId;
      containerId.set_value(Path(latest.get()).basename());
      state.latest = containerId;
    } else {
      ContainerID containerId;
      containerId.set_value(Path(path).basename());

      Try<RunState> run = RunState::recover(
          rootDir, slaveId, frameworkId, executorId, containerId, strict);

      if (run.isError()) {
        return Error(
            "Failed to recover run " + containerId.value() +
            " of executor '" + executorId.value() +
            "': " + run.error());
      }

      state.runs[containerId] = run.get();
      state.errors += run.get().errors;
    }
  }

  // Find the latest executor.
  // It is possible that we cannot find the "latest" executor if the
  // slave died before it created the "latest" symlink.
  if (state.latest.isNone()) {
    LOG(WARNING) << "Failed to find the latest run of executor '"
                 << executorId << "' of framework " << frameworkId;
    return state;
  }

  // Read the executor info.
  const string& path =
    paths::getExecutorInfoPath(rootDir, slaveId, frameworkId, executorId);
  if (!os::exists(path)) {
    // This could happen if the slave died after creating the executor
    // directory but before it checkpointed the executor info.
    LOG(WARNING) << "Failed to find executor info file '" << path << "'";
    return state;
  }

  const Result<ExecutorInfo>& executorInfo =
    ::protobuf::read<ExecutorInfo>(path);

  if (executorInfo.isError()) {
    message = "Failed to read executor info from '" + path + "': " +
              executorInfo.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (executorInfo.isNone()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty executor info file '" << path << "'";
    return state;
  }

  state.info = executorInfo.get();

  return state;
}


Try<RunState> RunState::recover(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    bool strict)
{
  RunState state;
  state.id = containerId;
  string message;

  // See if the sentinel file exists. This is done first so it is
  // known even if partial state is returned, e.g., if the libprocess
  // pid file is not recovered. It indicates the slave removed the
  // executor.
  string path = paths::getExecutorSentinelPath(
      rootDir, slaveId, frameworkId, executorId, containerId);

  state.completed = os::exists(path);

  // Find the tasks.
  Try<list<string> > tasks = paths::getTaskPaths(
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId);

  if (tasks.isError()) {
    return Error(
        "Failed to find tasks for executor run " + containerId.value() +
        ": " + tasks.error());
  }

  // Recover tasks.
  foreach (const string& path, tasks.get()) {
    TaskID taskId;
    taskId.set_value(Path(path).basename());

    Try<TaskState> task = TaskState::recover(
        rootDir, slaveId, frameworkId, executorId, containerId, taskId, strict);

    if (task.isError()) {
      return Error(
          "Failed to recover task " + taskId.value() + ": " + task.error());
    }

    state.tasks[taskId] = task.get();
    state.errors += task.get().errors;
  }

  // Read the forked pid.
  path = paths::getForkedPidPath(
      rootDir, slaveId, frameworkId, executorId, containerId);
  if (!os::exists(path)) {
    // This could happen if the slave died before the isolator
    // checkpointed the forked pid.
    LOG(WARNING) << "Failed to find executor forked pid file '" << path << "'";
    return state;
  }

  Try<string> pid = os::read(path);

  if (pid.isError()) {
    message = "Failed to read executor forked pid from '" + path +
              "': " + pid.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (pid.get().empty()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty executor forked pid file '" << path << "'";
    return state;
  }

  Try<pid_t> forkedPid = numify<pid_t>(pid.get());
  if (forkedPid.isError()) {
    return Error("Failed to parse forked pid " + pid.get() +
                 ": " + forkedPid.error());
  }

  state.forkedPid = forkedPid.get();

  // Read the libprocess pid.
  path = paths::getLibprocessPidPath(
      rootDir, slaveId, frameworkId, executorId, containerId);

  if (!os::exists(path)) {
    // This could happen if the slave died before the executor
    // registered with the slave.
    LOG(WARNING)
      << "Failed to find executor libprocess pid file '" << path << "'";
    return state;
  }

  pid = os::read(path);

  if (pid.isError()) {
    message = "Failed to read executor libprocess pid from '" + path +
              "': " + pid.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (pid.get().empty()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty executor libprocess pid file '" << path << "'";
    return state;
  }

  state.libprocessPid = process::UPID(pid.get());

  return state;
}


Try<TaskState> TaskState::recover(
    const string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId,
    bool strict)
{
  TaskState state;
  state.id = taskId;
  string message;

  // Read the task info.
  string path = paths::getTaskInfoPath(
      rootDir, slaveId, frameworkId, executorId, containerId, taskId);
  if (!os::exists(path)) {
    // This could happen if the slave died after creating the task
    // directory but before it checkpointed the task info.
    LOG(WARNING) << "Failed to find task info file '" << path << "'";
    return state;
  }

  const Result<Task>& task = ::protobuf::read<Task>(path);

  if (task.isError()) {
    message = "Failed to read task info from '" + path + "': " + task.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  if (task.isNone()) {
    // This could happen if the slave died after opening the file for
    // writing but before it checkpointed anything.
    LOG(WARNING) << "Found empty task info file '" << path << "'";
    return state;
  }

  state.info = task.get();

  // Read the status updates.
  path = paths::getTaskUpdatesPath(
      rootDir, slaveId, frameworkId, executorId, containerId, taskId);
  if (!os::exists(path)) {
    // This could happen if the slave died before it checkpointed any
    // status updates for this task.
    LOG(WARNING) << "Failed to find status updates file '" << path << "'";
    return state;
  }

  // Open the status updates file for reading and writing (for
  // truncating).
  Try<int> fd = os::open(path, O_RDWR | O_CLOEXEC);

  if (fd.isError()) {
    message = "Failed to open status updates file '" + path +
              "': " + fd.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  // Now, read the updates.
  Result<StatusUpdateRecord> record = None();
  while (true) {
    // Ignore errors due to partial protobuf read and enable undoing
    // failed reads by reverting to the previous seek position.
    record = ::protobuf::read<StatusUpdateRecord>(fd.get(), true, true);

    if (!record.isSome()) {
      break;
    }

    if (record.get().type() == StatusUpdateRecord::UPDATE) {
      state.updates.push_back(record.get().update());
    } else {
      state.acks.insert(UUID::fromBytes(record.get().uuid()));
    }
  }

  off_t offset = lseek(fd.get(), 0, SEEK_CUR);

  if (offset < 0) {
    os::close(fd.get());
    return ErrnoError("Failed to lseek status updates file '" + path + "'");
  }

  // Always truncate the file to contain only valid updates.
  // NOTE: This is safe even though we ignore partial protobuf read
  // errors above, because the 'fd' is properly set to the end of the
  // last valid update by 'protobuf::read()'.
  if (ftruncate(fd.get(), offset) != 0) {
    os::close(fd.get());
    return ErrnoError(
        "Failed to truncate status updates file '" + path + "'");
  }

  // After reading a non-corrupted updates file, 'record' should be
  // 'none'.
  if (record.isError()) {
    message = "Failed to read status updates file  '" + path +
              "': " + record.error();

    os::close(fd.get());

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  // Close the updates file.
  os::close(fd.get());

  return state;
}


Try<ResourcesState> ResourcesState::recover(
    const std::string& rootDir,
    bool strict)
{
  ResourcesState state;

  const string& path = paths::getResourcesInfoPath(rootDir);
  if (!os::exists(path)) {
    LOG(INFO) << "No checkpointed resources found at '" << path << "'";
    return state;
  }

  Try<int> fd = os::open(path, O_RDWR | O_CLOEXEC);
  if (fd.isError()) {
    string message =
      "Failed to open resources file '" + path + "': " + fd.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  Result<Resource> resource = None();
  while (true) {
    // Ignore errors due to partial protobuf read and enable undoing
    // failed reads by reverting to the previous seek position.
    resource = ::protobuf::read<Resource>(fd.get(), true, true);
    if (!resource.isSome()) {
      break;
    }

    state.resources += resource.get();
  }

  off_t offset = lseek(fd.get(), 0, SEEK_CUR);
  if (offset < 0) {
    os::close(fd.get());
    return ErrnoError("Failed to lseek resources file '" + path + "'");
  }

  // Always truncate the file to contain only valid resources.
  // NOTE: This is safe even though we ignore partial protobuf read
  // errors above, because the 'fd' is properly set to the end of the
  // last valid resource by 'protobuf::read()'.
  if (ftruncate(fd.get(), offset) != 0) {
    os::close(fd.get());
    return ErrnoError("Failed to truncate resources file '" + path + "'");
  }

  // After reading a non-corrupted resources file, 'record' should be
  // 'none'.
  if (resource.isError()) {
    string message =
      "Failed to read resources file  '" + path + "': " + resource.error();

    os::close(fd.get());

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  os::close(fd.get());

  return state;
}


} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
