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

#ifndef __STATUS_UPDATE_MANAGER_PROCESS_HPP__
#define __STATUS_UPDATE_MANAGER_PROCESS_HPP__

#include <list>
#include <queue>
#include <string>
#include <utility>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include <stout/os/ftruncate.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/constants.hpp"

namespace mesos {
namespace internal {

// `StatusUpdateManagerProcess` is responsible for
//
// 1) Reliably sending status updates.
// 2) Checkpointing updates to disk (optional).
// 3) Receiving ACKs.
// 4) Recovering checkpointed status updates after failover.
//
// It takes the following template parameters:
//  - `IDType` the type of the objects used to identify the managed streams.
//  - `CheckpointType` the type of the protobuf message written to checkpoint
//    the streams.
//  - `UpdateType` the type of the status updates that will be managed.
//
// NOTE: Unless first paused, this actor will forward updates as soon as
// possible; for example, during recovery or as soon as the first status update
// is processed.
//
// This process does NOT garbage collect any checkpointed state. The users of it
// are responsible for the garbage collection of the status updates files.
//
// TODO(gkleiman): make `TaskStatusUpdateManager` use this actor (MESOS-8296).
template <typename IDType, typename CheckpointType, typename UpdateType>
class StatusUpdateManagerProcess
  : public ProtobufProcess<
        StatusUpdateManagerProcess<IDType, CheckpointType, UpdateType>>
{
public:
  // This struct contains a map from stream ID to the stream state
  // recovered for the status updates file.
  //
  // The stream state will be `None` if:
  //
  //   * The status updates file didn't exist.
  //   * The status updates file was empty.
  //
  // The stream state contains all the status updates (both acknowledged and
  // pending) added to the stream.
  //
  // This struct also contains a count of the recoverable errors found during
  // non-strict recovery.
  struct State
  {
    struct StreamState
    {
      std::list<UpdateType> updates;
      bool terminated;

      StreamState() : updates(), terminated(false) {}
    };

    // The value will be `None` if the stream could not be recovered.
    hashmap<IDType, Option<StreamState>> streams;
    unsigned int errors;

    State() : streams(), errors(0) {}
  };

  StatusUpdateManagerProcess(
      const std::string& id,
      const std::string& _statusUpdateType)
    : process::ProcessBase(process::ID::generate(id)),
      statusUpdateType(_statusUpdateType),
      paused(false) {}

  StatusUpdateManagerProcess(const StatusUpdateManagerProcess& that) = delete;
  StatusUpdateManagerProcess& operator=(
      const StatusUpdateManagerProcess& that) = delete;

  // Implementation.

  // Explicitly use `initialize` since we're overloading below.
  using process::ProcessBase::initialize;

  // Initializes the actor with the necessary callbacks.
  //
  // `_forwardCallback` is called whenever there is a new status update that
  // needs to be forwarded.
  // `_getPath` is called in order to generate the path of a status update
  // stream checkpoint file, given an `IDType`.
  void initialize(
      const lambda::function<void(const UpdateType&)>& _forwardCallback,
      const lambda::function<const std::string(const IDType&)>& _getPath)
  {
    forwardCallback = _forwardCallback;
    getPath = _getPath;
  }

  // Forwards the status update on the specified update stream.
  //
  // If `checkpoint` is `false`, the update will be retried as long as it is in
  // memory, but it will not be checkpointed.
  process::Future<Nothing> update(
      const UpdateType& update,
      const IDType& streamId,
      bool checkpoint)
  {
    LOG(INFO) << "Received " << statusUpdateType << " " << update;

    if (!streams.contains(streamId)) {
      Try<Nothing> create =
        createStatusUpdateStream(
            streamId,
            update.has_framework_id()
              ? Option<FrameworkID>(update.framework_id())
              : None(),
            checkpoint);

      if (create.isError()) {
        return process::Failure(create.error());
      }
    }
    CHECK(streams.contains(streamId));
    StatusUpdateStream* stream = streams[streamId].get();

    if (update.has_latest_status()) {
      return process::Failure(
          "Expected " + statusUpdateType + " to not contain 'latest_status'");
    }

    // Verify that we didn't get a non-checkpointable update for a
    // stream that is checkpointable, and vice-versa.
    if (stream->checkpointed() != checkpoint) {
      return process::Failure(
          "Mismatched checkpoint value for " + statusUpdateType + " " +
          stringify(update) + " (expected checkpoint=" +
          stringify(stream->checkpointed()) + " actual checkpoint=" +
          stringify(checkpoint) + ")");
    }

    // Verify that the framework ID of the update matches the framework ID
    // of the stream.
    if (update.has_framework_id() != stream->frameworkId.isSome()) {
      return process::Failure(
          "Mismatched framework ID for " + statusUpdateType +
          " " + stringify(update) + " (expected " +
          (stream->frameworkId.isSome()
             ? stringify(stream->frameworkId.get())
             : "no framework ID") +
          " got " +
          (update.has_framework_id()
             ? stringify(update.framework_id())
             : "no framework ID") +
          ")");
    }

    if (update.has_framework_id() &&
        update.framework_id() != stream->frameworkId.get()) {
      return process::Failure(
          "Mismatched framework ID for " + statusUpdateType +
          " " + stringify(update) +
          " (expected " + stringify(stream->frameworkId.get()) +
          " actual " + stringify(update.framework_id()) + ")");
    }

    // Handle the status update.
    Try<bool> result = stream->update(update);
    if (result.isError()) {
      return process::Failure(result.error());
    }

    // This only happens if the status update is a duplicate.
    if (!result.get()) {
      return Nothing();
    }

    // Forward the status update if this is at the front of the queue.
    // Subsequent status updates will be sent in `acknowledgement()`.
    if (!paused && stream->pending.size() == 1) {
      CHECK_NONE(stream->timeout);

      const Result<UpdateType>& next = stream->next();
      if (next.isError()) {
        return process::Failure(next.error());
      }

      CHECK_SOME(next);
      stream->timeout =
        forward(stream, next.get(), slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
    }

    return Nothing();
  }

  // Process the acknowledgment of a status update.
  //
  // This will result in the next status update being forwarded.
  //
  // Returns `true` if the ACK is handled successfully (e.g., checkpointed)
  //                and the task's status update stream is not terminated.
  //         `false` same as above except the status update stream is
  //                terminated.
  //         `Failure` if there are any errors (e.g., duplicate, checkpointing).
  process::Future<bool> acknowledgement(
      const IDType& streamId,
      const id::UUID& uuid)
  {
    LOG(INFO) << "Received " << statusUpdateType
              << " acknowledgement (UUID: " << uuid << ")"
              << " for stream " << stringify(streamId);

    // This might happen if we haven't completed recovery yet or if the
    // acknowledgement is for a stream that has been cleaned up.
    if (!streams.contains(streamId)) {
      return process::Failure(
          "Cannot find the " + statusUpdateType + " stream " +
          stringify(streamId));
    }

    StatusUpdateStream* stream = streams[streamId].get();

    // Handle the acknowledgement.
    Try<bool> result = stream->acknowledgement(uuid);

    if (result.isError()) {
      return process::Failure(result.error());
    }

    if (!result.get()) {
      return process::Failure(
          "Duplicate " + statusUpdateType + " acknowledgement");
    }

    stream->timeout = None();

    // Get the next update in the queue.
    const Result<UpdateType>& next = stream->next();
    if (next.isError()) {
      return process::Failure(next.error());
    }

    bool terminated = stream->terminated;
    if (terminated) {
      if (next.isSome()) {
        LOG(WARNING) << "Acknowledged a terminal " << statusUpdateType
                     << " but updates are still pending";
      }
      cleanupStatusUpdateStream(streamId);
    } else if (!paused && next.isSome()) {
      // Forward the next queued status update.
      stream->timeout =
        forward(stream, next.get(), slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
    }

    return !terminated;
  }

  // Recovers the status update manager's state using the supplied stream IDs.
  //
  // Returns:
  //  - The recovered state if successful.
  //  - The recovered state, including the number of errors encountered, if
  //    `strict == false` and any of the streams couldn't be recovered.
  //  - A `Failure` if `strict == true` and any of the streams couldn't be
  //    recovered.
  process::Future<State> recover(
      const std::list<IDType>& streamIds,
      bool strict)
  {
    LOG(INFO) << "Recovering " << statusUpdateType << " manager";

    State state;
    foreach (const IDType& streamId, streamIds) {
      Result<typename StatusUpdateStream::State> result =
        recoverStatusUpdateStream(streamId, strict);

      if (result.isError()) {
        const std::string message =
          "Failed to recover " + statusUpdateType + " stream " +
          stringify(streamId) + ": " + result.error();
        LOG(WARNING) << message;

        if (strict) {
          foreachkey (const IDType& streamId, utils::copy(streams)) {
            cleanupStatusUpdateStream(streamId);
          }

          CHECK(streams.empty());
          CHECK(frameworkStreams.empty());

          return process::Failure(message);
        }

        state.errors++;
      } else if (result.isNone()) {
        // This can happen if the initial checkpoint of the stream didn't
        // complete.
        state.streams[streamId] = None();
      } else {
        const typename StatusUpdateStream::State& streamState = result.get();

        state.streams[streamId] = typename State::StreamState();
        state.streams[streamId]->updates = streamState.updates;
        state.streams[streamId]->terminated = streamState.terminated;

        if (streamState.error) {
          state.errors++;
        }
      }
    }

    return state;
  }

  // Closes all status update streams corresponding to a framework.
  //
  // NOTE: This stops retrying any pending status updates for this framework,
  // but does NOT garbage collect any checkpointed state. The caller is
  // responsible for garbage collection after this method has returned.
  void cleanup(const FrameworkID& frameworkId)
  {
    LOG(INFO) << "Closing " << statusUpdateType << " streams of framework "
              << frameworkId;

    if (frameworkStreams.contains(frameworkId)) {
      foreach (const IDType& streamId,
               utils::copy(frameworkStreams[frameworkId])) {
        cleanupStatusUpdateStream(streamId);
      }
    }
  }

  void pause()
  {
    LOG(INFO) << "Pausing " << statusUpdateType << " manager";
    paused = true;
  }

  void resume()
  {
    LOG(INFO) << "Resuming " << statusUpdateType << " manager";
    paused = false;

    foreachvalue (process::Owned<StatusUpdateStream>& stream, streams) {
      const Result<UpdateType>& next = stream->next();

      if (next.isSome()) {
        const UpdateType& update = next.get();

        LOG(INFO) << "Sending " << statusUpdateType << " " << update;

        stream->timeout = forward(
            stream.get(), update, slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
      }
    }
  }

private:
  // Forward declarations.
  class StatusUpdateStream;

  // Helper methods.

  // Creates a new status update stream, adding it to `streams`.
  Try<Nothing> createStatusUpdateStream(
      const IDType& streamId,
      const Option<FrameworkID>& frameworkId,
      bool checkpoint)
  {
    VLOG(1) << "Creating " << statusUpdateType << " stream "
            << stringify(streamId) << " checkpoint=" << stringify(checkpoint);

    Try<process::Owned<StatusUpdateStream>> stream =
      StatusUpdateStream::create(
          statusUpdateType,
          streamId,
          frameworkId,
          checkpoint ? Option<std::string>(getPath(streamId)) : None());

    if (stream.isError()) {
      return Error(stream.error());
    }

    streams[streamId] = std::move(stream.get());

    if (frameworkId.isSome()) {
      frameworkStreams[frameworkId.get()].insert(streamId);
    }

    return Nothing();
  }


  // Recovers a status update stream and adds it to the map of streams.
  Result<typename StatusUpdateStream::State> recoverStatusUpdateStream(
      const IDType& streamId,
      bool strict)
  {
    VLOG(1) << "Recovering " << statusUpdateType << " stream "
            << stringify(streamId);

    Result<std::pair<
        process::Owned<StatusUpdateStream>,
        typename StatusUpdateStream::State>> result =
          StatusUpdateStream::recover(
              statusUpdateType, streamId, getPath(streamId), strict);

    if (result.isError()) {
      return Error(result.error());
    }

    if (result.isNone()) {
      return None();
    }

    process::Owned<StatusUpdateStream> stream = std::get<0>(result.get());
    typename StatusUpdateStream::State& streamState = std::get<1>(result.get());

    if (stream->terminated) {
      return streamState;
    }

    if (stream->frameworkId.isSome()) {
      frameworkStreams[stream->frameworkId.get()].insert(streamId);
    }

    // Get the next update in the queue.
    const Result<UpdateType>& next = stream->next();
    if (next.isError()) {
      return Error(next.error());
    }

    if (!paused && next.isSome()) {
      // Forward the next queued status update.
      stream->timeout = forward(
          stream.get(), next.get(), slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
    }

    streams[streamId] = std::move(stream);

    return streamState;
  }

  void cleanupStatusUpdateStream(const IDType& streamId)
  {
    VLOG(1) << "Cleaning up " << statusUpdateType << " stream "
            << stringify(streamId);

    CHECK(streams.contains(streamId)) << "Cannot find " << statusUpdateType
                                      << " stream " << stringify(streamId);

    StatusUpdateStream* stream = streams[streamId].get();

    if (stream->frameworkId.isSome()) {
      const FrameworkID frameworkId = stream->frameworkId.get();

      CHECK(frameworkStreams.contains(frameworkId));

      frameworkStreams[frameworkId].erase(streamId);
      if (frameworkStreams[frameworkId].empty()) {
        frameworkStreams.erase(frameworkId);
      }
    }

    streams.erase(streamId);
  }

  // Forwards the status update and starts a timer based on the `duration` to
  // check for ACK.
  process::Timeout forward(
      StatusUpdateStream* stream,
      const UpdateType& _update,
      const Duration& duration)
  {
    CHECK(!paused);
    CHECK(!_update.has_latest_status());
    CHECK_NOTNULL(stream);

    UpdateType update(_update);
    update.mutable_latest_status()->CopyFrom(
        stream->pending.empty() ? _update.status()
                                : stream->pending.back().status());

    VLOG(1) << "Forwarding " << statusUpdateType << " " << update;

    forwardCallback(update);

    // Send a message to self to resend after some delay if no ACK is received.
    return delay(
        duration,
        ProtobufProcess<
            StatusUpdateManagerProcess<
            IDType,
            CheckpointType,
            UpdateType>>::self(),
        &StatusUpdateManagerProcess::timeout,
        stream->streamId,
        duration)
      .timeout();
  }

  // Status update timeout.
  void timeout(const IDType& streamId, const Duration& duration)
  {
    if (paused || !streams.contains(streamId)) {
      return;
    }

    StatusUpdateStream* stream = streams[streamId].get();

    // Check and see if we should resend the status update.
    if (!stream->pending.empty()) {
      CHECK_SOME(stream->timeout);

      if (stream->timeout->expired()) {
        const UpdateType& update = stream->pending.front();
        LOG(WARNING) << "Resending " << statusUpdateType << " " << update;

        // Bounded exponential backoff.
        Duration duration_ =
          std::min(duration * 2, slave::STATUS_UPDATE_RETRY_INTERVAL_MAX);

        stream->timeout = forward(stream, update, duration_);
      }
    }
  }

  // Type of status updates handled by the stream, e.g., "operation status
  // update".
  const std::string statusUpdateType;

  lambda::function<void(UpdateType)> forwardCallback;
  lambda::function<const std::string(const IDType&)> getPath;

  hashmap<IDType, process::Owned<StatusUpdateStream>> streams;
  hashmap<FrameworkID, hashset<IDType>> frameworkStreams;
  bool paused;

  // Handles the status updates and acknowledgements, checkpointing them if
  // necessary. It also holds the information about received, acknowledged and
  // pending status updates.
  class StatusUpdateStream
  {
  public:
    struct State
    {
      std::list<UpdateType> updates;

      bool error;
      bool terminated; // Set to `true` if a terminal status update was ACK'ed.

      State() : updates(), error(false), terminated(false) {}
    };

    ~StatusUpdateStream()
    {
      if (fd.isSome()) {
        Try<Nothing> close = os::close(fd.get());

        if (close.isError()) {
          CHECK_SOME(path);
          LOG(WARNING) << "Failed to close " << statusUpdateType
                       << " stream file '" << path.get() << "': "
                       << close.error();
        }
      }
    }

    static Try<process::Owned<StatusUpdateStream>> create(
        const std::string& statusUpdateType,
        const IDType& streamId,
        const Option<FrameworkID>& frameworkId,
        const Option<std::string>& path)
    {
      Option<int_fd> fd;

      if (path.isSome()) {
        if (os::exists(path.get())) {
          return Error("The file '" + path.get() + "' already exists");
        }

        // Create the base updates directory, if it doesn't exist.
        const std::string& dirName = Path(path.get()).dirname();
        Try<Nothing> directory = os::mkdir(dirName);
        if (directory.isError()) {
          return Error(
              "Failed to create '" + dirName + "': " + directory.error());
        }

        // Open the updates file.
        Try<int_fd> result = os::open(
            path.get(),
            O_CREAT | O_SYNC | O_WRONLY | O_CLOEXEC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

        if (result.isError()) {
          return Error(
              "Failed to open '" + path.get() + "' : " + result.error());
        }

        fd = result.get();
      }

      process::Owned<StatusUpdateStream> stream(
          new StatusUpdateStream(statusUpdateType, streamId, path, fd));

      stream->frameworkId = frameworkId;

      return std::move(stream);
    }


    static Result<std::pair<process::Owned<StatusUpdateStream>, State>> recover(
        const std::string& statusUpdateType,
        const IDType& streamId,
        const std::string& path,
        bool strict)
    {
      if (os::exists(Path(path).dirname()) && !os::exists(path)) {
        // This could happen if the process died before it checkpointed any
        // status updates.
        return None();
      }

      // Open the status updates file for reading and writing.
      Try<int_fd> fd = os::open(
          path,
#ifdef __WINDOWS__
          O_BINARY |
#endif // __WINDOWS__
          O_SYNC | O_RDWR | O_CLOEXEC);

      if (fd.isError()) {
        return Error("Failed to open '" + path + "': " + fd.error());
      }

      process::Owned<StatusUpdateStream> stream(
          new StatusUpdateStream(statusUpdateType, streamId, path, fd.get()));

      VLOG(1) << "Replaying " << statusUpdateType << " stream "
              << stringify(streamId);

      // Read the updates/acknowledgments, building both the stream's in-memory
      // structures and the state object which will be returned.

      State state;
      Result<CheckpointType> record = None();
      while (true) {
        // Ignore errors due to partial protobuf read and enable undoing failed
        // reads by reverting to the previous seek position.
        record = ::protobuf::read<CheckpointType>(fd.get(), true, true);

        if (!record.isSome()) {
          break;
        }

        switch (record->type()) {
          case CheckpointType::ACK: {
            // Get the corresponding update for this ACK.
            const Result<UpdateType>& update = stream->next();
            if (update.isError()) {
              return Error(update.error());
            }

            if (update.isNone()) {
              return Error(
                  "Unexpected " + statusUpdateType + " acknowledgment"
                  " (UUID: " + stringify(record->uuid()) +
                  ") for stream " + stringify(streamId));
            }
            stream->_handle(update.get(), record->type());
            break;
          }
          case CheckpointType::UPDATE: {
            stream->_handle(record->update(), record->type());
            state.updates.push_back(record->update());
            break;
          }
        }
      }

      // Always truncate the file to contain only valid updates.
      // NOTE: This is safe even though we ignore partial protobuf read
      // errors above, because the `fd` is properly set to the end of the
      // last valid update by `protobuf::read()`.
      Try<off_t> currentPosition = os::lseek(fd.get(), 0, SEEK_CUR);
      if (currentPosition.isError()) {
        return Error(
            "Failed to lseek file '" + path + "': " + currentPosition.error());
      }

      Try<Nothing> truncated = os::ftruncate(fd.get(), currentPosition.get());

      if (truncated.isError()) {
        return Error(
            "Failed to truncate file '" + path + "': " + truncated.error());
      }

      // After reading a non-corrupted updates file, `record` should be `none`.
      if (record.isError()) {
        std::string message =
          "Failed to read file '" + path + "': " + record.error();

        if (strict) {
          return Error(message);
        }

        LOG(WARNING) << message;
        state.error = true;
      }

      state.terminated = stream->terminated;

      if (state.updates.empty()) {
        // A stream is created only once there's something to write to it, so
        // this can only happen if the checkpointing of the first update was
        // interrupted.

        // On Windows you can only delete a file if it is not open. The
        // stream's destructor will close the file, so we need to destroy it
        // here.
        stream.reset();

        Try<Nothing> removed = os::rm(path);
        if (removed.isError()) {
          return Error(
              "Failed to remove file '" + path + "': " + removed.error());
        }

        return None();
      }

      return std::make_pair(stream, state);
    }

    // This function handles the update, checkpointing if necessary.
    //
    // Returns `true`:  if the update is successfully handled.
    //         `false`: if the update is a duplicate or has already been
    //                  acknowledged.
    //         `Error`: any errors (e.g., checkpointing).
    Try<bool> update(const UpdateType& update)
    {
      if (error.isSome()) {
        return Error(error.get());
      }

      if (!update.status().has_uuid()) {
        return Error("Status update is missing 'uuid'");
      }
      Try<id::UUID> uuid = id::UUID::fromBytes(update.status().uuid().value());
      CHECK_SOME(uuid);

      // Check that this status update has not already been acknowledged.
      if (acknowledged.contains(uuid.get())) {
        LOG(WARNING) << "Ignoring " << statusUpdateType << " " << update
                     << " that has already been acknowledged";
        return false;
      }

      // Check that this update has not already been received.
      if (received.contains(uuid.get())) {
        LOG(WARNING) << "Ignoring duplicate " << statusUpdateType << " "
                     << update;
        return false;
      }

      // Handle the update, checkpointing if necessary.
      Try<Nothing> result = handle(update, CheckpointType::UPDATE);
      if (result.isError()) {
        return Error(result.error());
      }

      return true;
    }

    // This function handles the ACK, checkpointing if necessary.
    //
    // Returns `true`: if the acknowledgement is successfully handled.
    //         `false`: if the acknowledgement is a duplicate.
    //         `Error`: Any errors (e.g., checkpointing).
    Try<bool> acknowledgement(const id::UUID& uuid)
    {
      if (error.isSome()) {
        return Error(error.get());
      }

      // Get the corresponding update for this ACK.
      const Result<UpdateType>& update_ = next();
      if (update_.isError()) {
        return Error(update_.error());
      }

      // This might happen if we retried a status update and got back
      // acknowledgments for both the original and the retried update.
      if (update_.isNone()) {
        return Error(
            "Unexpected acknowledgment (UUID: " + uuid.toString() +
            ") for " + statusUpdateType + " stream " + stringify(streamId));
      }

      const UpdateType& update = update_.get();

      if (acknowledged.contains(uuid)) {
        LOG(WARNING) << "Duplicate acknowledgment for " << statusUpdateType
                     << " " << update;
        return false;
      }

      Try<id::UUID> updateUuid =
        id::UUID::fromBytes(update.status().uuid().value());
      CHECK_SOME(updateUuid);

      // This might happen if we retried a status update and got back
      // acknowledgments for both the original and the retried update.
      if (uuid != updateUuid.get()) {
        LOG(WARNING) << "Unexpected " << statusUpdateType
                     << " acknowledgment (received " << uuid
                     << ", expecting " << updateUuid.get() << ") for "
                     << update;
        return false;
      }

      // Handle the ACK, checkpointing if necessary.
      Try<Nothing> result = handle(update, CheckpointType::ACK);
      if (result.isError()) {
        return Error(result.error());
      }

      return true;
    }

    // Returns the next update (or none, if empty) in the queue.
    Result<UpdateType> next()
    {
      if (error.isSome()) {
        return Error(error.get());
      }

      if (!pending.empty()) {
        return pending.front();
      }

      return None();
    }

    // Returns `true` if the stream is checkpointed, `false` otherwise.
    bool checkpointed() { return path.isSome(); }

    const IDType streamId;

    bool terminated;
    Option<FrameworkID> frameworkId;
    Option<process::Timeout> timeout; // Timeout for resending status update.
    std::queue<UpdateType> pending;

  private:
    StatusUpdateStream(
        const std::string& _statusUpdateType,
        const IDType& _streamId,
        const Option<std::string>& _path,
        Option<int_fd> _fd)
      : streamId(_streamId),
        terminated(false),
        statusUpdateType(_statusUpdateType),
        path(_path),
        fd(_fd) {}

    // Handles the status update and writes it to disk, if necessary.
    //
    // TODO(vinod): The write has to be asynchronous to avoid status updates
    // that are being checkpointed, blocking the processing of other updates.
    // One solution is to wrap the protobuf::write inside async, but it's
    // probably too much of an overhead to spin up a new libprocess per status
    // update?
    // A better solution might be to be have async write capability for file IO.
    Try<Nothing> handle(
        const UpdateType& update,
        const typename CheckpointType::Type& type)
    {
      CHECK_NONE(error);

      // Checkpoint the update if necessary.
      if (checkpointed()) {
        LOG(INFO) << "Checkpointing " << type << " for " << statusUpdateType
                  << " " << update;

        CHECK_SOME(fd);

        CheckpointType record;
        record.set_type(type);

        switch (type) {
          case CheckpointType::UPDATE:
            record.mutable_update()->CopyFrom(update);
            break;
          case CheckpointType::ACK:
            record.mutable_uuid()->CopyFrom(update.status().uuid());
            break;
        }

        Try<Nothing> write = ::protobuf::write(fd.get(), record);
        if (write.isError()) {
          error =
            "Failed to write to file '" + path.get() + "': " + write.error();
          return Error(error.get());
        }
      }

      // Now actually handle the update.
      _handle(update, type);

      return Nothing();
    }


    // Handles the status update without checkpointing.
    void _handle(
        const UpdateType& update,
        const typename CheckpointType::Type& type)
    {
      CHECK_NONE(error);

      Try<id::UUID> uuid = id::UUID::fromBytes(update.status().uuid().value());
      CHECK_SOME(uuid);

      switch (type) {
        case CheckpointType::UPDATE:
          if (update.has_framework_id()) {
            frameworkId = update.framework_id();
          }

          received.insert(uuid.get());

          // Add it to the pending updates queue.
          pending.push(update);
          break;
        case CheckpointType::ACK:
          acknowledged.insert(uuid.get());

          // Remove the corresponding update from the pending queue.
          pending.pop();

          if (!terminated) {
            terminated = protobuf::isTerminalState(update.status().state());
          }
          break;
      }
    }

    // Type of status updates handled by the stream, e.g., "operation status
    // update".
    const std::string& statusUpdateType;

    const Option<std::string> path; // File path of the update stream.
    const Option<int_fd> fd; // File descriptor to the update stream.

    hashset<id::UUID> received;
    hashset<id::UUID> acknowledged;

    Option<std::string> error; // Potential non-retryable error.
  };
};

} // namespace internal {
} // namespace mesos {

#endif // __STATUS_UPDATE_MANAGER_PROCESS_HPP__
