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

#include "common/protobuf_utils.hpp"

#include "stout/foreach.hpp"
#include "stout/hashmap.hpp"
#include "stout/hashset.hpp"
#include "stout/protobuf.hpp"
#include "stout/utils.hpp"

#include "slave/constants.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


namespace mesos {
namespace internal {
namespace slave {

class StatusUpdateManagerProcess
  : public ProtobufProcess<StatusUpdateManagerProcess>
{
public:
  StatusUpdateManagerProcess() {}
  virtual ~StatusUpdateManagerProcess();

  void initialize(const PID<Slave>& slave);

  Try<Nothing> update(
      const StatusUpdate& update,
      bool checkpoint,
      const Option<std::string>& path);

  Try<Nothing> acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const string& uuid);

  void newMasterDetected(const UPID& pid);

  void cleanup(const FrameworkID& frameworkId);

private:
  // Status update timeout.
  void timeout();

  // Forwards the status update to the master and starts a timer to check
  // for ACK from the scheduler.
  // NOTE: This should only be used for those messages that expect an
  // ACK (e.g updates from the executor).
  Timeout forward(const StatusUpdate& update);

  // Helper functions.

  // Creates a new status update stream (opening the updates file, if path is
  // present) and adds it to streams.
  StatusUpdateStream* createStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const Option<std::string>& path);

  StatusUpdateStream* getStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  void cleanupStatusUpdateStream(
      const TaskID& taskId,
      const FrameworkID& frameworkId);

  UPID master;
  PID<Slave> slave;
  hashmap<FrameworkID, hashmap<TaskID, StatusUpdateStream*> > streams;
};


StatusUpdateManagerProcess::~StatusUpdateManagerProcess()
{
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (StatusUpdateStream* stream, streams[frameworkId]) {
      delete stream;
    }
  }
  streams.clear();
}


void StatusUpdateManagerProcess::initialize(const process::PID<Slave>& _slave)
{
  slave = _slave;
}


void StatusUpdateManagerProcess::newMasterDetected(const UPID& pid)
{
  LOG(INFO) << "New master detected at " << pid;
  master = pid;
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


Try<Nothing> StatusUpdateManagerProcess::update(
    const StatusUpdate& update,
    bool checkpoint,
    const Option<string>& path)
{
  CHECK(!checkpoint || path.isSome())
    << "Asked to checkpoint update " << update << " without providing a path";

  const TaskID& taskId = update.status().task_id();
  const FrameworkID& frameworkId = update.framework_id();

  LOG(INFO) << "Received status update " << update;

  // Write the status update to disk and enqueue it to send it to the master.
  // Create/Get the status update stream for this task.
  StatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);
  if (stream == NULL) {
    stream = createStatusUpdateStream(taskId, frameworkId, path);
  }

  // Handle the status update.
  Try<Nothing> result = stream->update(update);
  if (result.isError()) {
    return result;
  }

  // Forward the status update to the master if this is the first in the stream.
  // Subsequent status updates will get sent in 'acknowledgement()'.
  if (stream->pending.size() == 1) {
    CHECK(stream->timeout.isNone());
    const Result<StatusUpdate>& next = stream->next();
    if (next.isError()) {
      return Error(next.error());
    }

    CHECK_SOME(next);
    stream->timeout = forward(next.get());
  }

  return Nothing();
}


Timeout StatusUpdateManagerProcess::forward(const StatusUpdate& update)
{
  LOG(INFO) << "Forwarding status update " << update
            << " to the master at " << master;

  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(slave); // The ACK will be first received by the slave.

  send(master, message);

  // Send a message to self to resend after some delay if no ACK is received.
  return delay(STATUS_UPDATE_RETRY_INTERVAL,
               self(),
               &StatusUpdateManagerProcess::timeout).timeout();
}


Try<Nothing> StatusUpdateManagerProcess::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const string& uuid)
{
  LOG(INFO) << "Received status update acknowledgement"
            << " for task " << taskId
            << " of framework " << frameworkId;

  StatusUpdateStream* stream = getStatusUpdateStream(taskId, frameworkId);

  // This might happen if we haven't completed recovery yet or if the
  // acknowledgement is for a stream that has been cleaned up.
  if (stream == NULL) {
    return Error(
        "Cannot find the status update stream for task " + stringify(taskId) +
        " of framework " + stringify(frameworkId));
  }

  // Get the corresponding update for this ACK.
  const Result<StatusUpdate>& update = stream->next();
  if (update.isError()) {
    return Error(update.error());
  }

  // This might happen if we retried a status update and got back
  // acknowledgments for both the original and the retried update.
  if (update.isNone()) {
    LOG(WARNING) << "Ignoring unexpected status update acknowledgment"
                 << " for task " << taskId
                 << " of framework " << frameworkId;
    return Nothing();
  }

  // Handle the acknowledgement.
  Try<Nothing> result =
    stream->acknowledgement(taskId, frameworkId, uuid, update.get());

  if (result.isError()) {
    return result;
  }

  // Reset the timeout.
  stream->timeout = None();

  // Get the next update in the queue.
  const Result<StatusUpdate>& next = stream->next();
  if (next.isError()) {
    return Error(next.error());
  }

  if (protobuf::isTerminalState(update.get().status().state())) {
    if (next.isSome()) {
      LOG(WARNING) << "Acknowledged a terminal"
                   << " status update " << update.get()
                   << " but updates are still pending";
    }
    cleanupStatusUpdateStream(taskId, frameworkId);
  } else if (next.isSome()) {
    // Forward the next queued status update.
    stream->timeout = forward(next.get());
  }

  return Nothing();
}


// TODO(vinod): There should be a limit on the retries.
void StatusUpdateManagerProcess::timeout()
{
  LOG(INFO) << "Checking for unacknowledged status updates";
  // Check and see if we should resend any status updates.
  foreachkey (const FrameworkID& frameworkId, streams) {
    foreachvalue (StatusUpdateStream* stream, streams[frameworkId]) {
      CHECK_NOTNULL(stream);
      if (!stream->pending.empty()) {
        CHECK(stream->timeout.isSome());
        if (stream->timeout.get().expired()) {
          const StatusUpdate& update = stream->pending.front();
          LOG(WARNING) << "Resending status update " << update;
          stream->timeout = forward(update);
        }
      }
    }
  }
}


StatusUpdateStream* StatusUpdateManagerProcess::createStatusUpdateStream(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const Option<string>& path)
{
  LOG(INFO) << "Creating StatusUpdate stream for task " << taskId
            << " of framework " << frameworkId;

  StatusUpdateStream* stream =
    new StatusUpdateStream(taskId, frameworkId, path);

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
  LOG(INFO) << "Cleaning up status update stream"
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


StatusUpdateManager::StatusUpdateManager()
{
  process = new StatusUpdateManagerProcess();
  spawn(process);
}


StatusUpdateManager::~StatusUpdateManager()
{
  terminate(process);
  wait(process);
  delete process;
}


void StatusUpdateManager::initialize(const PID<Slave>& slave)
{
  dispatch(process, &StatusUpdateManagerProcess::initialize, slave);
}


Future<Try<Nothing> > StatusUpdateManager::update(
    const StatusUpdate& update,
    bool checkpoint,
    const Option<std::string>& path)
{
  return dispatch(
      process, &StatusUpdateManagerProcess::update, update, checkpoint, path);
}


Future<Try<Nothing> > StatusUpdateManager::acknowledgement(
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const std::string& uuid)
{
  return dispatch(
      process,
      &StatusUpdateManagerProcess::acknowledgement,
      taskId,
      frameworkId,
      uuid);
}


void StatusUpdateManager::newMasterDetected(const UPID& pid)
{
  dispatch(process, &StatusUpdateManagerProcess::newMasterDetected, pid);
}


void StatusUpdateManager::cleanup(const FrameworkID& frameworkId)
{
  dispatch(process, &StatusUpdateManagerProcess::cleanup, frameworkId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
