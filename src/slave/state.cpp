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
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "slave/paths.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

using std::list;
using std::string;
using std::max;


Result<SlaveState> recover(const string& rootDir, bool strict)
{
  LOG(INFO) << "Recovering state from '" << rootDir << "'";

  // We consider the absence of 'rootDir' to mean that this is either
  // the first time this slave was started with checkpointing enabled
  // or this slave was started after an upgrade (--recover=cleanup).
  if (!os::exists(rootDir)) {
    return None();
  }

  // Did the machine reboot?
  if (os::exists(paths::getBootIdPath(rootDir))) {
    Try<string> read = os::read(paths::getBootIdPath(rootDir));
    if (read.isSome()) {
      Try<string> id = os::bootId();
      CHECK_SOME(id);

      if (id.get() != strings::trim(read.get())) {
        LOG(INFO) << "Slave host rebooted";
        return None();
      }
    }
  }

  const std::string& latest = paths::getLatestSlavePath(rootDir);

  // Check if the "latest" symlink to a slave directory exists.
  if (!os::exists(latest)) {
    // The slave was asked to shutdown or died before it registered
    // and had a chance to create the "latest" symlink.
    LOG(INFO) << "Failed to find the latest slave from '" << rootDir << "'";
    return None();
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
  slaveId.set_value(os::basename(directory.get()).get());

  Try<SlaveState> state = SlaveState::recover(rootDir, slaveId, strict);
  if (state.isError()) {
    return Error(state.error());
  }

  return state.get();
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
    // This could happen if the slave died before it registered
    // with the master.
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
  const Try<list<string> >& frameworks = os::glob(
      strings::format(paths::FRAMEWORK_PATH, rootDir, slaveId, "*").get());

  if (frameworks.isError()) {
    return Error("Failed to find frameworks for slave " + slaveId.value() +
                 ": " + frameworks.error());
  }

  // Recover each of the frameworks.
  foreach (const string& path, frameworks.get()) {
    FrameworkID frameworkId;
    frameworkId.set_value(os::basename(path).get());

    const Try<FrameworkState>& framework =
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
    // framework directory but before it checkpointed the
    // framework info.
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

  const Try<string>& pid = os::read(path);

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
  const Try<list<string> >& executors = os::glob(strings::format(
      paths::EXECUTOR_PATH, rootDir, slaveId, frameworkId, "*").get());

  if (executors.isError()) {
    return Error(
        "Failed to find executors for framework " + frameworkId.value() +
        ": " + executors.error());
  }

   // Recover the executors.
  foreach (const string& path, executors.get()) {
    ExecutorID executorId;
    executorId.set_value(os::basename(path).get());

    const Try<ExecutorState>& executor =
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

  // Find the runs.
  const Try<list<string> >& runs = os::glob(strings::format(
      paths::EXECUTOR_RUN_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      "*").get());

  if (runs.isError()) {
    return Error("Failed to find runs for executor '" + executorId.value() +
                 "': " + runs.error());
  }

  // Recover the runs.
  foreach (const string& path, runs.get()) {
    if (os::basename(path).get() == paths::LATEST_SYMLINK) {
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
      containerId.set_value(os::basename(latest.get()).get());
      state.latest = containerId;
    } else {
      ContainerID containerId;
      containerId.set_value(os::basename(path).get());

      const Try<RunState>& run = RunState::recover(
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

  // Find the tasks.
  const Try<list<string> >& tasks = os::glob(strings::format(
      paths::TASK_PATH,
      rootDir,
      slaveId,
      frameworkId,
      executorId,
      containerId,
      "*").get());

  if (tasks.isError()) {
    return Error(
        "Failed to find tasks for executor run " + containerId.value() +
        ": " + tasks.error());
  }

  // Recover tasks.
  foreach (const string& path, tasks.get()) {
    TaskID taskId;
    taskId.set_value(os::basename(path).get());

    const Try<TaskState>& task = TaskState::recover(
        rootDir, slaveId, frameworkId, executorId, containerId, taskId, strict);

    if (task.isError()) {
      return Error(
          "Failed to recover task " + taskId.value() + ": " + task.error());
    }

    state.tasks[taskId] = task.get();
    state.errors += task.get().errors;
  }

  // Read the forked pid.
  string path = paths::getForkedPidPath(
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

  // See if the sentinel file exists.
  path = paths::getExecutorSentinelPath(
      rootDir, slaveId, frameworkId, executorId, containerId);

  state.completed = os::exists(path);

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
    // This could happen if the slave died before it checkpointed
    // any status updates for this task.
    LOG(WARNING) << "Failed to find status updates file '" << path << "'";
    return state;
  }

  // Open the status updates file for reading and writing (for truncating).
  const Try<int>& fd = os::open(path, O_RDWR);

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
    // Ignore errors due to partial protobuf read.
    record = ::protobuf::read<StatusUpdateRecord>(fd.get(), true);

    if (!record.isSome()) {
      break;
    }

    if (record.get().type() == StatusUpdateRecord::UPDATE) {
      state.updates.push_back(record.get().update());
    } else {
      state.acks.insert(UUID::fromBytes(record.get().uuid()));
    }
  }

  // Always truncate the file to contain only valid updates.
  // NOTE: This is safe even though we ignore partial protobuf
  // read errors above, because the 'fd' is properly set to the
  // end of the last valid update by 'protobuf::read()'.
  if (ftruncate(fd.get(), lseek(fd.get(), 0, SEEK_CUR)) != 0) {
    return ErrnoError(
        "Failed to truncate status updates file '" + path + "'");
  }

  // After reading a non-corrupted updates file, 'record' should be 'none'.
  if (record.isError()) {
    message = "Failed to read status updates file  '" + path +
              "': " + record.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  // Close the updates file.
  const Try<Nothing>& close = os::close(fd.get());

  if (close.isError()) {
    message = "Failed to close status updates file '" + path +
              "': " + close.error();

    if (strict) {
      return Error(message);
    } else {
      LOG(WARNING) << message;
      state.errors++;
      return state;
    }
  }

  return state;
}


// Helpers to checkpoint string/protobuf to disk, with necessary error checking.

Try<Nothing> checkpoint(
    const string& path,
    const google::protobuf::Message& message)
{
  // Create the base directory.
  Try<Nothing> result = os::mkdir(os::dirname(path).get());
  if (result.isError()) {
    return Error("Failed to create directory '" + os::dirname(path).get() +
                 "': " + result.error());
  }

  // Now checkpoint the protobuf to disk.
  result = ::protobuf::write(path, message);
  if (result.isError()) {
    return Error("Failed to checkpoint \n" + message.DebugString() +
                 "\n to '" + path + "': " + result.error());
  }

  return Nothing();
}


Try<Nothing> checkpoint(const std::string& path, const std::string& message)
{
  // Create the base directory.
  Try<Nothing> result = os::mkdir(os::dirname(path).get());
  if (result.isError()) {
    return Error("Failed to create directory '" + os::dirname(path).get() +
                 "': " + result.error());
  }

  // Now checkpoint the message to disk.
  result = os::write(path, message);
  if (result.isError()) {
    return Error("Failed to checkpoint '" + message + "' to '" + path +
                 "': " + result.error());
  }

  return Nothing();
}

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
