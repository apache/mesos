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

#include "slave/task_status_update_manager.hpp"

#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "logging/logging.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

using lambda::function;

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.
using process::Failure;
using process::Future;
using process::PID;
using process::Timeout;
using process::UPID;

namespace mesos {
namespace internal {
namespace slave {

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;
using state::TaskState;


class TaskStatusUpdateManagerProcess
  : public ProtobufProcess<TaskStatusUpdateManagerProcess>
{
public:
  TaskStatusUpdateManagerProcess(const Flags& flags);
  ~TaskStatusUpdateManagerProcess() override;

  // Explicitly use 'initialize' since we're overloading below.
  using process::ProcessBase::initialize;

  // TaskStatusUpdateManager implementation.
  void initialize(const function<void(StatusUpdate)>& forward);

  Future<Nothing> update(
      const StatusUpdate& update,
      const SlaveID& slaveId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  Future<Nothing> update(
      const StatusUpdate& update,
      const SlaveID& slaveId);

  Future<bool> acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const id::UUID& uuid);

  Future<Nothing> recover(
      const string& rootDir,
      const Option<SlaveState>& state);

  void pause();
  void resume();

  void cleanup(const FrameworkID& frameworkId);

private:
  // Helper function to handle update.
  Future<Nothing> _update(
      const StatusUpdate& update,
      const SlaveID& slaveId,
      bool checkpoint,
      const Option<ExecutorID>& executorId,
      const Option<ContainerID>& containerId);

  // Status update timeout.
  void timeout(const Duration& duration);

  // Forwards the status update to the master and starts a timer based
  // on the 'duration' to check for ACK from the scheduler.
  // NOTE: This should only be used for those messages that expect an
  // ACK (e.g updates from the executor).
  Timeout forward(const StatusUpdate& update, const Duration& duration);

  // Helper functions.

  // Creates a new status update stream (opening the updates file, if path is
  // present) and adds it to streams.
  TaskStatusUpdateStream* createStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      bool checkpoint,
      const Option<ExecutorID>& executorId,
      const Option<ContainerID>& containerId);

  TaskStatusUpdateStream* getStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  void cleanupStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  const Flags flags;
  bool paused;

  function<void(StatusUpdate)> forward_;

  hashmap<FrameworkID, hashmap<TaskID, TaskStatusUpdateStream*>> streams;
};


TaskStatusUpdateManagerProcess::TaskStatusUpdateManagerProcess(
    const Flags& _flags)
  : ProcessBase(process::ID::generate("task-status-update-manager")),
    flags(_flags),
    paused(false)
{
}


TaskStatusUpdateManagerProcess::~TaskStatusUpdateManagerProcess()
{
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (TaskStatusUpdateStream* stream, streams[frameworkId]) {
      delete stream;
    }
  }
  streams.clear();
}


void TaskStatusUpdateManagerProcess::initialize(
    const function<void(StatusUpdate)>& forward)
{
  forward_ = forward;
}


void TaskStatusUpdateManagerProcess::pause()
{
  LOG(INFO) << "Pausing sending task status updates";
  paused = true;
}


void TaskStatusUpdateManagerProcess::resume()
{
  LOG(INFO) << "Resuming sending task status updates";
  paused = false;

  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (TaskStatusUpdateStream* stream, streams[frameworkId]) {
      if (!stream->pending.empty()) {
        const StatusUpdate& update = stream->pending.front();
        LOG(WARNING) << "Resending task status update " << update;
        stream->timeout = forward(update, STATUS_UPDATE_RETRY_INTERVAL_MIN);
      }
    }
  }
}


Future<Nothing> TaskStatusUpdateManagerProcess::recover(
    const string& rootDir,
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering task status update manager";

  if (state.isNone()) {
    return Nothing();
  }

  foreachvalue (const FrameworkState& framework, state->frameworks) {
    foreachvalue (const ExecutorState& executor, framework.executors) {
      LOG(INFO) << "Recovering executor '" << executor.id
                << "' of framework " << framework.id;

      if (executor.info.isNone()) {
        LOG(WARNING) << "Skipping recovering task status updates of"
                     << " executor '" << executor.id
                     << "' of framework " << framework.id
                     << " because its info cannot be recovered";
        continue;
      }

      if (executor.latest.isNone()) {
        LOG(WARNING) << "Skipping recovering task status updates of"
                     << " executor '" << executor.id
                     << "' of framework " << framework.id
                     << " because its latest run cannot be recovered";
        continue;
      }

      // We are only interested in the latest run of the executor!
      const ContainerID& latest = executor.latest.get();
      Option<RunState> run = executor.runs.get(latest);
      CHECK_SOME(run);

      if (run->completed) {
        VLOG(1) << "Skipping recovering task status updates of"
                << " executor '" << executor.id
                << "' of framework " << framework.id
                << " because its latest run " << latest.value()
                << " is completed";
        continue;
      }

      foreachvalue (const TaskState& task, run->tasks) {
        // No updates were ever received for this task!
        // This means either:
        // 1) the executor never received this task or
        // 2) executor launched it but the slave died before it got any updates.
        if (task.updates.empty()) {
          LOG(WARNING) << "No status updates found for task " << task.id
                       << " of framework " << framework.id;
          continue;
        }

        // Create a new status update stream.
        TaskStatusUpdateStream* stream = createStatusUpdateStream(
            task.id, framework.id, state->id, true, executor.id, latest);

        // Replay the stream.
        Try<Nothing> replay = stream->replay(task.updates, task.acks);
        if (replay.isError()) {
          return Failure(
              "Failed to replay status updates for task " + stringify(task.id) +
              " of framework " + stringify(framework.id) +
              ": " + replay.error());
        }

        // At the end of the replay, the stream is either terminated or
        // contains only unacknowledged, if any, pending updates. The
        // pending updates will be flushed after the slave
        // reregisters with the master.
        if (stream->terminated) {
          cleanupStatusUpdateStream(task.id, framework.id);
        }
      }
    }
  }

  return Nothing();
}


void TaskStatusUpdateManagerProcess::cleanup(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Closing task status update streams for framework "
            << frameworkId;

  if (streams.contains(frameworkId)) {
    foreachkey (const TaskID& taskId, utils::copy(streams[frameworkId])) {
      cleanupStatusUpdateStream(taskId, frameworkId);
    }
  }
}


Future<Nothing> TaskStatusUpdateManagerProcess::update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return _update(update, slaveId, true, executorId, containerId);
}


Future<Nothing> TaskStatusUpdateManagerProcess::update(
    const StatusUpdate& update,
    const SlaveID& slaveId)
{
  return _update(update, slaveId, false, None(), None());
}


Future<Nothing> TaskStatusUpdateManagerProcess::_update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    bool checkpoint,
    const Option<ExecutorID>& executorId,
    const Option<ContainerID>& containerId)
{
  const TaskID& taskId = update.status().task_id();
  const FrameworkID& frameworkId = update.framework_id();

  LOG(INFO) << "Received task status update " << update;

  // Write the status update to disk and enqueue it to send it to the master.
  // Create/Get the status update stream for this task.
  TaskStatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);
  if (stream == nullptr) {
    stream = createStatusUpdateStream(
        taskId, frameworkId, slaveId, checkpoint, executorId, containerId);
  }

  // Verify that we didn't get a non-checkpointable update for a
  // stream that is checkpointable, and vice-versa.
  if (stream->checkpoint != checkpoint) {
    return Failure(
        "Mismatched checkpoint value for task status update " +
        stringify(update) + " (expected checkpoint=" +
        stringify(stream->checkpoint) + " actual checkpoint=" +
        stringify(checkpoint) + ")");
  }

  // Handle the status update.
  Try<bool> result = stream->update(update);
  if (result.isError()) {
    return Failure(result.error());
  }

  // We don't return a failed future here so that the slave can re-ack
  // the duplicate update.
  if (!result.get()) {
    return Nothing();
  }

  // Forward the status update to the master if this is the first in the stream.
  // Subsequent status updates will get sent in 'acknowledgement()'.
  if (!paused && stream->pending.size() == 1) {
    CHECK_NONE(stream->timeout);
    const Result<StatusUpdate>& next = stream->next();
    if (next.isError()) {
      return Failure(next.error());
    }

    CHECK_SOME(next);
    stream->timeout = forward(next.get(), STATUS_UPDATE_RETRY_INTERVAL_MIN);
  }

  return Nothing();
}


Timeout TaskStatusUpdateManagerProcess::forward(
    const StatusUpdate& update,
    const Duration& duration)
{
  CHECK(!paused);

  VLOG(1) << "Forwarding task status update " << update << " to the agent";

  // Forward the update.
  forward_(update);

  // Send a message to self to resend after some delay if no ACK is received.
  return delay(duration,
               self(),
               &TaskStatusUpdateManagerProcess::timeout,
               duration).timeout();
}


Future<bool> TaskStatusUpdateManagerProcess::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const id::UUID& uuid)
{
  LOG(INFO) << "Received task status update acknowledgement (UUID: " << uuid
            << ") for task " << taskId
            << " of framework " << frameworkId;

  TaskStatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);

  // This might happen if we haven't completed recovery yet or if the
  // acknowledgement is for a stream that has been cleaned up.
  if (stream == nullptr) {
    return Failure(
        "Cannot find the task status update stream for task " +
        stringify(taskId) + " of framework " + stringify(frameworkId));
  }

  // Get the corresponding update for this ACK.
  const Result<StatusUpdate>& update = stream->next();
  if (update.isError()) {
    return Failure(update.error());
  }

  // This might happen if we retried a status update and got back
  // acknowledgments for both the original and the retried update.
  if (update.isNone()) {
    return Failure(
        "Unexpected task status update acknowledgment (UUID: " +
        uuid.toString() + ") for task " + stringify(taskId) + " of framework " +
        stringify(frameworkId));
  }

  // Handle the acknowledgement.
  Try<bool> result =
    stream->acknowledgement(taskId, frameworkId, uuid, update.get());

  if (result.isError()) {
    return Failure(result.error());
  }

  if (!result.get()) {
    return Failure("Duplicate task status acknowledgement");
  }

  // Reset the timeout.
  stream->timeout = None();

  // Get the next update in the queue.
  const Result<StatusUpdate>& next = stream->next();
  if (next.isError()) {
    return Failure(next.error());
  }

  bool terminated = stream->terminated;

  if (terminated) {
    if (next.isSome()) {
      LOG(WARNING) << "Acknowledged a terminal"
                   << " task status update " << update.get()
                   << " but updates are still pending";
    }
    cleanupStatusUpdateStream(taskId, frameworkId);
  } else if (!paused && next.isSome()) {
    // Forward the next queued status update.
    stream->timeout = forward(next.get(), STATUS_UPDATE_RETRY_INTERVAL_MIN);
  }

  return !terminated;
}


// TODO(vinod): There should be a limit on the retries.
void TaskStatusUpdateManagerProcess::timeout(const Duration& duration)
{
  if (paused) {
    return;
  }

  // Check and see if we should resend any status updates.
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (TaskStatusUpdateStream* stream, streams[frameworkId]) {
      CHECK_NOTNULL(stream);
      if (!stream->pending.empty()) {
        CHECK_SOME(stream->timeout);
        if (stream->timeout->expired()) {
          const StatusUpdate& update = stream->pending.front();
          LOG(WARNING) << "Resending task status update " << update;

          // Bounded exponential backoff.
          Duration duration_ =
            std::min(duration * 2, STATUS_UPDATE_RETRY_INTERVAL_MAX);

          stream->timeout = forward(update, duration_);
        }
      }
    }
  }
}


TaskStatusUpdateStream*
TaskStatusUpdateManagerProcess::createStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    bool checkpoint,
    const Option<ExecutorID>& executorId,
    const Option<ContainerID>& containerId)
{
  VLOG(1) << "Creating StatusUpdate stream for task " << taskId
          << " of framework " << frameworkId;

  TaskStatusUpdateStream* stream = new TaskStatusUpdateStream(
      taskId, frameworkId, slaveId, flags, checkpoint, executorId, containerId);

  streams[frameworkId][taskId] = stream;
  return stream;
}


TaskStatusUpdateStream* TaskStatusUpdateManagerProcess::getStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId)
{
  if (!streams.contains(frameworkId)) {
    return nullptr;
  }

  if (!streams[frameworkId].contains(taskId)) {
    return nullptr;
  }

  return streams[frameworkId][taskId];
}


void TaskStatusUpdateManagerProcess::cleanupStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId)
{
  VLOG(1) << "Cleaning up status update stream"
          << " for task " << taskId
          << " of framework " << frameworkId;

  CHECK(streams.contains(frameworkId))
    << "Cannot find the task status update streams for framework "
    << frameworkId;

  CHECK(streams[frameworkId].contains(taskId))
    << "Cannot find the status update streams for task " << taskId;

  TaskStatusUpdateStream* stream = streams[frameworkId][taskId];

  streams[frameworkId].erase(taskId);
  if (streams[frameworkId].empty()) {
    streams.erase(frameworkId);
  }

  delete stream;
}


TaskStatusUpdateManager::TaskStatusUpdateManager(const Flags& flags)
{
  process = new TaskStatusUpdateManagerProcess(flags);
  spawn(process);
}


TaskStatusUpdateManager::~TaskStatusUpdateManager()
{
  terminate(process);
  wait(process);
  delete process;
}


void TaskStatusUpdateManager::initialize(
    const function<void(StatusUpdate)>& forward)
{
  dispatch(process, &TaskStatusUpdateManagerProcess::initialize, forward);
}


Future<Nothing> TaskStatusUpdateManager::update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return dispatch(
      process,
      &TaskStatusUpdateManagerProcess::update,
      update,
      slaveId,
      executorId,
      containerId);
}


Future<Nothing> TaskStatusUpdateManager::update(
    const StatusUpdate& update,
    const SlaveID& slaveId)
{
  return dispatch(
      process,
      &TaskStatusUpdateManagerProcess::update,
      update,
      slaveId);
}


Future<bool> TaskStatusUpdateManager::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const id::UUID& uuid)
{
  return dispatch(
      process,
      &TaskStatusUpdateManagerProcess::acknowledgement,
      taskId,
      frameworkId,
      uuid);
}


Future<Nothing> TaskStatusUpdateManager::recover(
    const string& rootDir,
    const Option<SlaveState>& state)
{
  return dispatch(
      process, &TaskStatusUpdateManagerProcess::recover, rootDir, state);
}


void TaskStatusUpdateManager::pause()
{
  dispatch(process, &TaskStatusUpdateManagerProcess::pause);
}


void TaskStatusUpdateManager::resume()
{
  dispatch(process, &TaskStatusUpdateManagerProcess::resume);
}


void TaskStatusUpdateManager::cleanup(const FrameworkID& frameworkId)
{
  dispatch(process, &TaskStatusUpdateManagerProcess::cleanup, frameworkId);
}


TaskStatusUpdateStream::TaskStatusUpdateStream(
    const TaskID& _taskId,
    const FrameworkID& _frameworkId,
    const SlaveID& _slaveId,
    const Flags& _flags,
    bool _checkpoint,
    const Option<ExecutorID>& executorId,
    const Option<ContainerID>& containerId)
    : checkpoint(_checkpoint),
      terminated(false),
      taskId(_taskId),
      frameworkId(_frameworkId),
      slaveId(_slaveId),
      flags(_flags),
      error(None())
{
  if (checkpoint) {
    CHECK_SOME(executorId);
    CHECK_SOME(containerId);

    path = paths::getTaskUpdatesPath(
        paths::getMetaRootDir(flags.work_dir),
        slaveId,
        frameworkId,
        executorId.get(),
        containerId.get(),
        taskId);

    // Create the base updates directory, if it doesn't exist.
    const string& dirName = Path(path.get()).dirname();
    Try<Nothing> directory = os::mkdir(dirName);
    if (directory.isError()) {
      error = "Failed to create '" + dirName + "': " + directory.error();
      return;
    }

    // Open the updates file.
    // NOTE: We don't use `O_SYNC` here because we only read this file
    // if the host did not crash. `os::write` success implies the kernel
    // will have flushed our data to the page cache. This is sufficient
    // for the recovery scenarios we use this data for.
    Try<int_fd> result = os::open(
        path.get(),
        O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (result.isError()) {
      error = "Failed to open '" + path.get() +
              "' for status updates: " + result.error();
      return;
    }

    // We keep the file open through the lifetime of the task, because it
    // makes it easy to append status update records to the file.
    fd = result.get();
  }
}


TaskStatusUpdateStream::~TaskStatusUpdateStream()
{
  if (fd.isSome()) {
    Try<Nothing> close = os::close(fd.get());
    if (close.isError()) {
      CHECK_SOME(path);
      LOG(ERROR) << "Failed to close file '" << path.get() << "': "
                 << close.error();
    }
  }
}


Try<bool> TaskStatusUpdateStream::update(const StatusUpdate& update)
{
  if (error.isSome()) {
    return Error(error.get());
  }

  if (!update.has_uuid()) {
    return Error("Task status update is missing 'uuid'");
  }

  // Check that this status update has not already been acknowledged.
  // This could happen in the rare case when the slave received the ACK
  // from the framework, died, but slave's ACK to the executor never made it!
  if (acknowledged.contains(id::UUID::fromBytes(update.uuid()).get())) {
    LOG(WARNING) << "Ignoring task status update " << update
                 << " that has already been acknowledged by the framework!";
    return false;
  }

  // Check that this update hasn't already been received.
  // This could happen if the slave receives a status update from an executor,
  // then crashes after it writes it to disk but before it sends an ack.
  if (received.contains(id::UUID::fromBytes(update.uuid()).get())) {
    LOG(WARNING) << "Ignoring duplicate task status update " << update;
    return false;
  }

  // Handle the update, checkpointing if necessary.
  Try<Nothing> result = handle(update, StatusUpdateRecord::UPDATE);
  if (result.isError()) {
    return Error(result.error());
  }

  return true;
}


Try<bool> TaskStatusUpdateStream::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const id::UUID& uuid,
    const StatusUpdate& update)
{
  if (error.isSome()) {
    return Error(error.get());
  }

  if (acknowledged.contains(uuid)) {
    LOG(WARNING) << "Duplicate task status update acknowledgment (UUID: "
                  << uuid << ") for update " << update;
    return false;
  }

  // This might happen if we retried a status update and got back
  // acknowledgments for both the original and the retried update.
  if (uuid != id::UUID::fromBytes(update.uuid()).get()) {
    LOG(WARNING) << "Unexpected task status update acknowledgement (received "
                 << uuid << ", expecting "
                 << id::UUID::fromBytes(update.uuid()).get()
                 << ") for update " << update;
    return false;
  }

  // Handle the ACK, checkpointing if necessary.
  Try<Nothing> result = handle(update, StatusUpdateRecord::ACK);
  if (result.isError()) {
    return Error(result.error());
  }

  return true;
}


Result<StatusUpdate> TaskStatusUpdateStream::next()
{
  if (error.isSome()) {
    return Error(error.get());
  }

  if (!pending.empty()) {
    return pending.front();
  }

  return None();
}


Try<Nothing> TaskStatusUpdateStream::replay(
    const std::vector<StatusUpdate>& updates,
    const hashset<id::UUID>& acks)
{
  if (error.isSome()) {
    return Error(error.get());
  }

  VLOG(1) << "Replaying task status update stream for task " << taskId;

  foreach (const StatusUpdate& update, updates) {
    // Handle the update.
    _handle(update, StatusUpdateRecord::UPDATE);

    // Check if the update has an ACK too.
    if (acks.contains(id::UUID::fromBytes(update.uuid()).get())) {
      _handle(update, StatusUpdateRecord::ACK);
    }
  }

  return Nothing();
}


Try<Nothing> TaskStatusUpdateStream::handle(
    const StatusUpdate& update,
    const StatusUpdateRecord::Type& type)
{
  CHECK_NONE(error);

  // Checkpoint the update if necessary.
  if (checkpoint) {
    LOG(INFO) << "Checkpointing " << type << " for task status update "
              << update;

    CHECK_SOME(fd);

    StatusUpdateRecord record;
    record.set_type(type);

    if (type == StatusUpdateRecord::UPDATE) {
      record.mutable_update()->CopyFrom(update);
    } else {
      record.set_uuid(update.uuid());
    }

    Try<Nothing> write = ::protobuf::write(fd.get(), record);
    if (write.isError()) {
      error = "Failed to write task status update " + stringify(update) +
              " to '" + path.get() + "': " + write.error();
      return Error(error.get());
    }
  }

  // Now actually handle the update.
  _handle(update, type);

  return Nothing();
}


void TaskStatusUpdateStream::_handle(
    const StatusUpdate& update,
    const StatusUpdateRecord::Type& type)
{
  CHECK_NONE(error);

  if (type == StatusUpdateRecord::UPDATE) {
    // Record this update.
    received.insert(id::UUID::fromBytes(update.uuid()).get());

    // Add it to the pending updates queue.
    pending.push(update);
  } else {
    // Record this ACK.
    acknowledged.insert(id::UUID::fromBytes(update.uuid()).get());

    // Remove the corresponding update from the pending queue.
    pending.pop();

    if (!terminated) {
      terminated = protobuf::isTerminalState(update.status().state());
    }
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
