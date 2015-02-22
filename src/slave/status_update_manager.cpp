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

#include <process/delay.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"
#include "slave/status_update_manager.hpp"

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


class StatusUpdateManagerProcess
  : public ProtobufProcess<StatusUpdateManagerProcess>
{
public:
  StatusUpdateManagerProcess(const Flags& flags);
  virtual ~StatusUpdateManagerProcess();

  // Explicitely use 'initialize' since we're overloading below.
  using process::ProcessBase::initialize;

  // StatusUpdateManager implementation.
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
      const UUID& uuid);

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
  StatusUpdateStream* createStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      bool checkpoint,
      const Option<ExecutorID>& executorId,
      const Option<ContainerID>& containerId);

  StatusUpdateStream* getStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  void cleanupStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  const Flags flags;
  bool paused;

  function<void(StatusUpdate)> forward_;

  hashmap<FrameworkID, hashmap<TaskID, StatusUpdateStream*> > streams;
};


StatusUpdateManagerProcess::StatusUpdateManagerProcess(const Flags& _flags)
  : flags(_flags), paused(false) {}


StatusUpdateManagerProcess::~StatusUpdateManagerProcess()
{
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (StatusUpdateStream* stream, streams[frameworkId]) {
      delete stream;
    }
  }
  streams.clear();
}


void StatusUpdateManagerProcess::initialize(
    const function<void(StatusUpdate)>& forward)
{
  forward_ = forward;
}


void StatusUpdateManagerProcess::pause()
{
  LOG(INFO) << "Pausing sending status updates";
  paused = true;
}


void StatusUpdateManagerProcess::resume()
{
  LOG(INFO) << "Resuming sending status updates";
  paused = false;

  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (StatusUpdateStream* stream, streams[frameworkId]) {
      if (!stream->pending.empty()) {
        const StatusUpdate& update = stream->pending.front();
        LOG(WARNING) << "Resending status update " << update;
        stream->timeout = forward(update, STATUS_UPDATE_RETRY_INTERVAL_MIN);
      }
    }
  }
}


Future<Nothing> StatusUpdateManagerProcess::recover(
    const string& rootDir,
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering status update manager";

  if (state.isNone()) {
    return Nothing();
  }

  foreachvalue (const FrameworkState& framework, state.get().frameworks) {
    foreachvalue (const ExecutorState& executor, framework.executors) {
      LOG(INFO) << "Recovering executor '" << executor.id
                << "' of framework " << framework.id;

      if (executor.info.isNone()) {
        LOG(WARNING) << "Skipping recovering updates of"
                     << " executor '" << executor.id
                     << "' of framework " << framework.id
                     << " because its info cannot be recovered";
        continue;
      }

      if (executor.latest.isNone()) {
        LOG(WARNING) << "Skipping recovering updates of"
                     << " executor '" << executor.id
                     << "' of framework " << framework.id
                     << " because its latest run cannot be recovered";
        continue;
      }

      // We are only interested in the latest run of the executor!
      const ContainerID& latest = executor.latest.get();
      Option<RunState> run = executor.runs.get(latest);
      CHECK_SOME(run);

      if (run.get().completed) {
        VLOG(1) << "Skipping recovering updates of"
                << " executor '" << executor.id
                << "' of framework " << framework.id
                << " because its latest run " << latest.value()
                << " is completed";
        continue;
      }

      foreachvalue (const TaskState& task, run.get().tasks) {
        // No updates were ever received for this task!
        // This means either:
        // 1) the executor never received this task or
        // 2) executor launched it but the slave died before it got any updates.
        if (task.updates.empty()) {
          LOG(WARNING) << "No updates found for task " << task.id
                       << " of framework " << framework.id;
          continue;
        }

        // Create a new status update stream.
        StatusUpdateStream* stream = createStatusUpdateStream(
            task.id, framework.id, state.get().id, true, executor.id, latest);

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
        // re-registers with the master.
        if (stream->terminated) {
          cleanupStatusUpdateStream(task.id, framework.id);
        }
      }
    }
  }

  return Nothing();
}


void StatusUpdateManagerProcess::cleanup(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Closing status update streams for framework " << frameworkId;

  if (streams.contains(frameworkId)) {
    foreachkey (const TaskID& taskId, utils::copy(streams[frameworkId])) {
      cleanupStatusUpdateStream(taskId, frameworkId);
    }
  }
}


Future<Nothing> StatusUpdateManagerProcess::update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return _update(update, slaveId, true, executorId, containerId);
}


Future<Nothing> StatusUpdateManagerProcess::update(
    const StatusUpdate& update,
    const SlaveID& slaveId)
{
  return _update(update, slaveId, false, None(), None());
}


Future<Nothing> StatusUpdateManagerProcess::_update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    bool checkpoint,
    const Option<ExecutorID>& executorId,
    const Option<ContainerID>& containerId)
{
  const TaskID& taskId = update.status().task_id();
  const FrameworkID& frameworkId = update.framework_id();

  LOG(INFO) << "Received status update " << update;

  // Write the status update to disk and enqueue it to send it to the master.
  // Create/Get the status update stream for this task.
  StatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);
  if (stream == NULL) {
    stream = createStatusUpdateStream(
        taskId, frameworkId, slaveId, checkpoint, executorId, containerId);
  }

  // Verify that we didn't get a non-checkpointable update for a
  // stream that is checkpointable, and vice-versa.
  if (stream->checkpoint != checkpoint) {
    return Failure(
        "Mismatched checkpoint value for status update " + stringify(update) +
        " (expected checkpoint=" + stringify(stream->checkpoint) +
        " actual checkpoint=" + stringify(checkpoint) + ")");
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
    CHECK(stream->timeout.isNone());
    const Result<StatusUpdate>& next = stream->next();
    if (next.isError()) {
      return Failure(next.error());
    }

    CHECK_SOME(next);
    stream->timeout = forward(next.get(), STATUS_UPDATE_RETRY_INTERVAL_MIN);
  }

  return Nothing();
}


Timeout StatusUpdateManagerProcess::forward(
    const StatusUpdate& update,
    const Duration& duration)
{
  CHECK(!paused);

  VLOG(1) << "Forwarding update " << update << " to the slave";

  // Forward the update.
  forward_(update);

  // Send a message to self to resend after some delay if no ACK is received.
  return delay(duration,
               self(),
               &StatusUpdateManagerProcess::timeout,
               duration).timeout();
}


Future<bool> StatusUpdateManagerProcess::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const UUID& uuid)
{
  LOG(INFO) << "Received status update acknowledgement (UUID: " << uuid
            << ") for task " << taskId
            << " of framework " << frameworkId;

  StatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);

  // This might happen if we haven't completed recovery yet or if the
  // acknowledgement is for a stream that has been cleaned up.
  if (stream == NULL) {
    return Failure(
        "Cannot find the status update stream for task " + stringify(taskId) +
        " of framework " + stringify(frameworkId));
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
        "Unexpected status update acknowledgment (UUID: " + uuid.toString() +
        ") for task " + stringify(taskId) +
        " of framework " + stringify(frameworkId));
  }

  // Handle the acknowledgement.
  Try<bool> result =
    stream->acknowledgement(taskId, frameworkId, uuid, update.get());

  if (result.isError()) {
    return Failure(result.error());
  }

  if (!result.get()) {
    return Failure("Duplicate acknowledgement");
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
                   << " status update " << update.get()
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
void StatusUpdateManagerProcess::timeout(const Duration& duration)
{
  if (paused) {
    return;
  }

  // Check and see if we should resend any status updates.
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (StatusUpdateStream* stream, streams[frameworkId]) {
      CHECK_NOTNULL(stream);
      if (!stream->pending.empty()) {
        CHECK(stream->timeout.isSome());
        if (stream->timeout.get().expired()) {
          const StatusUpdate& update = stream->pending.front();
          LOG(WARNING) << "Resending status update " << update;

          // Bounded exponential backoff.
          Duration duration_ =
            std::min(duration * 2, STATUS_UPDATE_RETRY_INTERVAL_MAX);

          stream->timeout = forward(update, duration_);
        }
      }
    }
  }
}


StatusUpdateStream* StatusUpdateManagerProcess::createStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    bool checkpoint,
    const Option<ExecutorID>& executorId,
    const Option<ContainerID>& containerId)
{
  VLOG(1) << "Creating StatusUpdate stream for task " << taskId
          << " of framework " << frameworkId;

  StatusUpdateStream* stream = new StatusUpdateStream(
      taskId, frameworkId, slaveId, flags, checkpoint, executorId, containerId);

  streams[frameworkId][taskId] = stream;
  return stream;
}


StatusUpdateStream* StatusUpdateManagerProcess::getStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId)
{
  if (!streams.contains(frameworkId)) {
    return NULL;
  }

  if (!streams[frameworkId].contains(taskId)) {
    return NULL;
  }

  return streams[frameworkId][taskId];
}


void StatusUpdateManagerProcess::cleanupStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId)
{
  VLOG(1) << "Cleaning up status update stream"
          << " for task " << taskId
          << " of framework " << frameworkId;

  CHECK(streams.contains(frameworkId))
    << "Cannot find the status update streams for framework " << frameworkId;

  CHECK(streams[frameworkId].contains(taskId))
    << "Cannot find the status update streams for task " << taskId;

  StatusUpdateStream* stream = streams[frameworkId][taskId];

  streams[frameworkId].erase(taskId);
  if (streams[frameworkId].empty()) {
    streams.erase(frameworkId);
  }

  delete stream;
}


StatusUpdateManager::StatusUpdateManager(const Flags& flags)
{
  process = new StatusUpdateManagerProcess(flags);
  spawn(process);
}


StatusUpdateManager::~StatusUpdateManager()
{
  terminate(process);
  wait(process);
  delete process;
}


void StatusUpdateManager::initialize(
    const function<void(StatusUpdate)>& forward)
{
  dispatch(process, &StatusUpdateManagerProcess::initialize, forward);
}


Future<Nothing> StatusUpdateManager::update(
    const StatusUpdate& update,
    const SlaveID& slaveId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  return dispatch(
      process,
      &StatusUpdateManagerProcess::update,
      update,
      slaveId,
      executorId,
      containerId);
}


Future<Nothing> StatusUpdateManager::update(
    const StatusUpdate& update,
    const SlaveID& slaveId)
{
  return dispatch(
      process,
      &StatusUpdateManagerProcess::update,
      update,
      slaveId);
}


Future<bool> StatusUpdateManager::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const UUID& uuid)
{
  return dispatch(
      process,
      &StatusUpdateManagerProcess::acknowledgement,
      taskId,
      frameworkId,
      uuid);
}


Future<Nothing> StatusUpdateManager::recover(
    const string& rootDir,
    const Option<SlaveState>& state)
{
  return dispatch(
      process, &StatusUpdateManagerProcess::recover, rootDir, state);
}


void StatusUpdateManager::pause()
{
  dispatch(process, &StatusUpdateManagerProcess::pause);
}


void StatusUpdateManager::resume()
{
  dispatch(process, &StatusUpdateManagerProcess::resume);
}


void StatusUpdateManager::cleanup(const FrameworkID& frameworkId)
{
  dispatch(process, &StatusUpdateManagerProcess::cleanup, frameworkId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
