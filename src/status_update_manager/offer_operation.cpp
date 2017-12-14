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

#include "status_update_manager/offer_operation.hpp"

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

OfferOperationStatusUpdateManager::OfferOperationStatusUpdateManager()
  : process(
        new StatusUpdateManagerProcess<
            id::UUID,
            OfferOperationStatusUpdateRecord,
            OfferOperationStatusUpdate>(
                "offer-operation-status-update-manager",
                "offer operation status update"))
{
  spawn(process.get());
}


OfferOperationStatusUpdateManager::~OfferOperationStatusUpdateManager()
{
  terminate(process.get());
  wait(process.get());
}


void OfferOperationStatusUpdateManager::initialize(
    const function<void(const OfferOperationStatusUpdate&)>& forward,
    const function<const std::string(const id::UUID&)>& getPath)
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::initialize,
      forward,
      getPath);
}


Future<Nothing> OfferOperationStatusUpdateManager::update(
    const OfferOperationStatusUpdate& update,
    bool checkpoint)
{
  Try<id::UUID> operationUuid =
    id::UUID::fromBytes(update.operation_uuid().value());
  CHECK_SOME(operationUuid);

  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::update,
      update,
      operationUuid.get(),
      checkpoint);
}


Future<bool> OfferOperationStatusUpdateManager::acknowledgement(
      const id::UUID& operationUuid,
      const id::UUID& statusUuid)
{
  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::acknowledgement,
      operationUuid,
      statusUuid);
}


process::Future<OfferOperationStatusManagerState>
OfferOperationStatusUpdateManager::recover(
    const std::list<id::UUID>& operationUuids,
    bool strict)
{
  return dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::recover,
      operationUuids,
      strict);
}


void OfferOperationStatusUpdateManager::cleanup(const FrameworkID& frameworkId)
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::cleanup,
      frameworkId);
}


void OfferOperationStatusUpdateManager::pause()
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::pause);
}


void OfferOperationStatusUpdateManager::resume()
{
  dispatch(
      process.get(),
      &StatusUpdateManagerProcess<
          id::UUID,
          OfferOperationStatusUpdateRecord,
          OfferOperationStatusUpdate>::resume);
}

} // namespace internal {
} // namespace mesos {
