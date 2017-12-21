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

#ifndef __STATUS_UPDATE_MANAGER_OPERATION_HPP__
#define __STATUS_UPDATE_MANAGER_OPERATION_HPP__

#include <list>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

#include "status_update_manager/status_update_manager_process.hpp"

namespace mesos {
namespace internal {

typedef StatusUpdateManagerProcess<
    id::UUID,
    UpdateOperationStatusRecord,
    UpdateOperationStatusMessage>::State OperationStatusUpdateManagerState;

class OperationStatusUpdateManager
{
public:
  // NOTE: Unless first paused, the status update manager will forward updates
  // as soon as possible; for example, during recovery or as soon as the first
  // status update is processed.
  OperationStatusUpdateManager();

  ~OperationStatusUpdateManager();

  OperationStatusUpdateManager(
      const OperationStatusUpdateManager& that) = delete;
  OperationStatusUpdateManager& operator=(
      const OperationStatusUpdateManager& that) = delete;

  // Expects two callbacks:
  //   `forward`: called in order to forward an operation status update to its
  //              recipient.
  //   `getPath`: called in order to generate the path of a status update stream
  //              file, given the operation's `operation_uuid`.
  void initialize(
      const lambda::function<
          void(const UpdateOperationStatusMessage&)>& forward,
      const lambda::function<const std::string(const id::UUID&)>& getPath);

  // Checkpoints the update if necessary and reliably sends the update.
  //
  // Returns whether the update is handled successfully (e.g. checkpointed).
  process::Future<Nothing> update(
      const UpdateOperationStatusMessage& update,
      bool checkpoint = true);

  // Checkpoints the acknowledgement to disk if necessary.
  // Also, sends the next pending status update, if any.
  //
  // Returns:
  //   - `true`: if the ACK is handled successfully (e.g., checkpointed)
  //             and the status update stream is not terminated.
  //   - `false`: same as above except the status update stream is terminated.
  //   - A `Failure`: if there are any errors (e.g., duplicate, checkpointing).
  process::Future<bool> acknowledgement(
      const id::UUID& operationUuid,
      const id::UUID& statusUuid);

  // Recover status updates. The provided list of operation_uuids is used as the
  // source of truth for which checkpointed files should be recovered from.
  //
  // Returns the recovered state, including a map from operation ID to the
  // stream state recovered for the status file.
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
  process::Future<OperationStatusUpdateManagerState> recover(
      const std::list<id::UUID>& operationUuids,
      bool strict);

  // Closes all the status update streams corresponding to this framework.
  //
  // NOTE: This stops retrying any pending status updates for this framework,
  // but does NOT garbage collect any checkpointed state. The caller is
  // responsible for garbage collection after this method has returned.
  void cleanup(const FrameworkID& frameworkId);

  // Stop forwarding status updates until `resume()` is called.
  void pause();

  // Resume forwarding status updates until `pause()` is called.
  void resume();

private:
  process::Owned<
      StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __STATUS_UPDATE_MANAGER_OPERATION_HPP__
