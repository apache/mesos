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

#ifndef __STATUS_UPDATE_MANAGER_HPP__
#define __STATUS_UPDATE_MANAGER_HPP__

#include <ostream>
#include <queue>
#include <string>
#include <utility>

#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.

namespace state {

struct SlaveState;

}

class StatusUpdateManagerProcess;
struct StatusUpdateStream;


// StatusUpdateManager is responsible for
// 1) Reliably sending status updates to the master.
// 2) Checkpointing the update to disk (optional).
// 3) Sending ACKs to the executor (optional).
// 4) Receiving ACKs from the scheduler.
class StatusUpdateManager
{
public:
  StatusUpdateManager();
  virtual ~StatusUpdateManager();

  void initialize(
      const Flags& flags,
      const PID<Slave>& slave);

  // TODO(vinod): Come up with better names/signatures for the
  // checkpointing and non-checkpointing 'update()' functions.
  // Currently, it is not obvious that one version of 'update()'
  // does checkpointing while the other doesn't.

  // Checkpoints the status update and reliably sends the
  // update to the master (and hence the scheduler).
  // @return Whether the update is handled successfully
  // (e.g. checkpointed).
  process::Future<Nothing> update(
      const StatusUpdate& update,
      const SlaveID& slaveId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  // Retries the update to the master (as long as the slave is
  // alive), but does not checkpoint the update.
  // @return Whether the update is handled successfully.
  process::Future<Nothing> update(
      const StatusUpdate& update,
      const SlaveID& slaveId);

  // Checkpoints the status update to disk if necessary.
  // Also, sends the next pending status update, if any.
  // @return True if the ACK is handled successfully (e.g., checkpointed)
  //              and the task's status update stream is not terminated.
  //         False same as above except the status update stream is terminated.
  //         Failed if there are any errors (e.g., duplicate, checkpointing).
  process::Future<bool> acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const UUID& uuid);

  // Recover status updates.
  process::Future<Nothing> recover(
      const std::string& rootDir,
      const Option<state::SlaveState>& state);

  // TODO(vinod): Remove this hack once the new leader detector code is merged.
  void newMasterDetected(const UPID& pid);

  // Resend all the pending updates right away.
  // This is useful when the updates were pending because there was
  // no master elected (e.g., during recovery) or framework failed over.
  void flush();

  // Closes all the status update streams corresponding to this framework.
  // NOTE: This stops retrying any pending status updates for this framework.
  void cleanup(const FrameworkID& frameworkId);

private:
  StatusUpdateManagerProcess* process;
};


// StatusUpdateStream handles the status updates and acknowledgements
// of a task, checkpointing them if necessary. It also holds the information
// about received, acknowledged and pending status updates.
// NOTE: A task is expected to have a globally unique ID across the lifetime
// of a framework. In other words the tuple (taskId, frameworkId) should be
// always unique.
struct StatusUpdateStream
{
  StatusUpdateStream(const TaskID& _taskId,
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
      Try<Nothing> directory = os::mkdir(os::dirname(path.get()).get());
      if (directory.isError()) {
        error = "Failed to create " + os::dirname(path.get()).get();
        return;
      }

      // Open the updates file.
      Try<int> result = os::open(
          path.get(),
          O_CREAT | O_WRONLY | O_APPEND | O_SYNC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

      if (result.isError()) {
        error = "Failed to open '" + path.get() + "' for status updates";
        return;
      }

      // We keep the file open through the lifetime of the task, because it
      // makes it easy to append status update records to the file.
      fd = result.get();
    }
  }

  ~StatusUpdateStream()
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

  // This function handles the update, checkpointing if necessary.
  // @return   True if the update is successfully handled.
  //           False if the update is a duplicate.
  //           Error Any errors (e.g., checkpointing).
  Try<bool> update(const StatusUpdate& update)
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    // Check that this status update has not already been acknowledged.
    // This could happen in the rare case when the slave received the ACK
    // from the framework, died, but slave's ACK to the executor never made it!
    if (acknowledged.contains(UUID::fromBytes(update.uuid()))) {
      LOG(WARNING) << "Ignoring status update " << update
                   << " that has already been acknowledged by the framework!";
      return false;
    }

    // Check that this update hasn't already been received.
    // This could happen if the slave receives a status update from an executor,
    // then crashes after it writes it to disk but before it sends an ack.
    if (received.contains(UUID::fromBytes(update.uuid()))) {
      LOG(WARNING) << "Ignoring duplicate status update " << update;
      return false;
    }

    // Handle the update, checkpointing if necessary.
    Try<Nothing> result = handle(update, StatusUpdateRecord::UPDATE);
    if (result.isError()) {
      return Error(result.error());
    }

    return true;
  }

  // This function handles the ACK, checkpointing if necessary.
  // @return   True if the acknowledgement is successfully handled.
  //           False if the acknowledgement is a duplicate.
  //           Error Any errors (e.g., checkpointing).
  Try<bool> acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const UUID& uuid,
      const StatusUpdate& update)
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    if (acknowledged.contains(uuid)) {
      LOG(WARNING) << "Duplicate status update acknowledgment (UUID: "
                    << uuid << ") for update " << update;
      return false;
    }

    // This might happen if we retried a status update and got back
    // acknowledgments for both the original and the retried update.
    if (uuid != UUID::fromBytes(update.uuid())) {
      LOG(WARNING) << "Unexpected status update acknowledgement (received "
                   << uuid << ", expecting " << UUID::fromBytes(update.uuid())
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

  // Returns the next update (or none, if empty) in the queue.
  Result<StatusUpdate> next()
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    if (!pending.empty()) {
      return pending.front();
    }

    return None();
  }

  // Replays the stream by sequentially handling an update and its
  // corresponding ACK, if present.
  Try<Nothing> replay(
      const std::vector<StatusUpdate>& updates,
      const hashset<UUID>& acks)
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    VLOG(1) << "Replaying status update stream for task " << taskId;

    foreach (const StatusUpdate& update, updates) {
      // Handle the update.
      _handle(update, StatusUpdateRecord::UPDATE);

      // Check if the update has an ACK too.
      if (acks.contains(UUID::fromBytes(update.uuid()))) {
        _handle(update, StatusUpdateRecord::ACK);
      }
    }

    return Nothing();
  }

  // TODO(vinod): Explore semantics to make these private.
  const bool checkpoint;
  bool terminated;
  Option<Timeout> timeout; // Timeout for resending status update.
  std::queue<StatusUpdate> pending;

private:
  // Handles the status update and writes it to disk, if necessary.
  // TODO(vinod): The write has to be asynchronous to avoid status updates that
  // are being checkpointed, blocking the processing of other updates.
  // One solution is to wrap the protobuf::write inside async, but its probably
  // too much of an overhead to spin up a new libprocess per status update?
  // A better solution might be to be have async write capability for file io.
  Try<Nothing> handle(
      const StatusUpdate& update,
      const StatusUpdateRecord::Type& type)
  {
    CHECK(error.isNone());

    // Checkpoint the update if necessary.
    if (checkpoint) {
      LOG(INFO) << "Checkpointing " << type << " for status update " << update;

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
        error = "Failed to write status update " + stringify(update) +
                " to '" + path.get() + "': " + write.error();
        return Error(error.get());
      }
    }

    // Now actually handle the update.
    _handle(update, type);

    return Nothing();
  }

  void _handle(const StatusUpdate& update, const StatusUpdateRecord::Type& type)
  {
    CHECK(error.isNone());

    if (type == StatusUpdateRecord::UPDATE) {
      // Record this update.
      received.insert(UUID::fromBytes(update.uuid()));

      // Add it to the pending updates queue.
      pending.push(update);
    } else {
      // Record this ACK.
      acknowledged.insert(UUID::fromBytes(update.uuid()));

      // Remove the corresponding update from the pending queue.
      pending.pop();

      if (!terminated) {
        terminated = protobuf::isTerminalState(update.status().state());
      }
    }
  }

  const TaskID taskId;
  const FrameworkID frameworkId;
  const SlaveID slaveId;

  const Flags flags;

  hashset<UUID> received;
  hashset<UUID> acknowledged;

  Option<std::string> path; // File path of the update stream.
  Option<int> fd; // File descriptor to the update stream.

  Option<std::string> error; // Potential non-retryable error.
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {


#endif // __STATUS_UPDATE_MANAGER_HPP__
