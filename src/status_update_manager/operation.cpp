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

#include "status_update_manager/operation.hpp"

#include <list>

#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/uuid.hpp>

using lambda::function;

using process::Future;
using process::Owned;
using process::wait; // Necessary on some OS's to disambiguate.

namespace mesos {
namespace internal {

OperationStatusUpdateManager::OperationStatusUpdateManager()
  : process(
        new StatusUpdateManagerProcess<
            id::UUID,
            UpdateOperationStatusRecord,
            UpdateOperationStatusMessage>(
                "operation-status-update-manager",
                "operation status update"))
{
  spawn(process.get());
}


OperationStatusUpdateManager::~OperationStatusUpdateManager()
{
  terminate(process.get());
  wait(process.get());
}


void OperationStatusUpdateManager::initialize(
    const function<void(const UpdateOperationStatusMessage&)>& forward,
    const function<const std::string(const id::UUID&)>& getPath)
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::initialize,
      forward,
      getPath);
}


Future<Nothing> OperationStatusUpdateManager::update(
    const UpdateOperationStatusMessage& update,
    bool checkpoint)
{
  Try<id::UUID> operationUuid =
    id::UUID::fromBytes(update.operation_uuid().value());
  CHECK_SOME(operationUuid);

  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::update,
      update,
      operationUuid.get(),
      checkpoint);
}


Future<bool> OperationStatusUpdateManager::acknowledgement(
      const id::UUID& operationUuid,
      const id::UUID& statusUuid)
{
  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::acknowledgement,
      operationUuid,
      statusUuid);
}


process::Future<OperationStatusUpdateManagerState>
OperationStatusUpdateManager::recover(
    const std::list<id::UUID>& operationUuids,
    bool strict)
{
  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::recover,
      operationUuids,
      strict);
}


void OperationStatusUpdateManager::cleanup(const FrameworkID& frameworkId)
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::cleanup,
      frameworkId);
}


void OperationStatusUpdateManager::pause()
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::pause);
}


void OperationStatusUpdateManager::resume()
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::resume);
}

} // namespace internal {
} // namespace mesos {
