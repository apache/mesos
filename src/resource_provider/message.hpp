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

#ifndef __RESOURCE_PROVIDER_MESSAGE_HPP__
#define __RESOURCE_PROVIDER_MESSAGE_HPP__

#include <ostream>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/jsonify.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {

struct ResourceProviderMessage
{
  enum class Type
  {
    UPDATE_STATE,
    UPDATE_OFFER_OPERATION_STATUS,
    DISCONNECT
  };

  struct UpdateState
  {
    ResourceProviderInfo info;
    id::UUID resourceVersion;
    Resources totalResources;
    hashmap<id::UUID, OfferOperation> offerOperations;
  };

  struct UpdateOfferOperationStatus
  {
    OfferOperationStatusUpdate update;
  };

  struct Disconnect
  {
    ResourceProviderID resourceProviderId;
  };

  Type type;

  Option<UpdateState> updateState;
  Option<UpdateOfferOperationStatus> updateOfferOperationStatus;
  Option<Disconnect> disconnect;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const ResourceProviderMessage& resourceProviderMessage)
{
  switch (resourceProviderMessage.type) {
    case ResourceProviderMessage::Type::UPDATE_STATE: {
      const Option<ResourceProviderMessage::UpdateState>&
        updateState = resourceProviderMessage.updateState;

      CHECK_SOME(updateState);

      return stream
          << "UPDATE_STATE: "
          << updateState->info.id() << " "
          << updateState->totalResources;
    }

    case ResourceProviderMessage::Type::UPDATE_OFFER_OPERATION_STATUS: {
      const Option<ResourceProviderMessage::UpdateOfferOperationStatus>&
        updateOfferOperationStatus =
          resourceProviderMessage.updateOfferOperationStatus;

      CHECK_SOME(updateOfferOperationStatus);

      return stream
          << "UPDATE_OFFER_OPERATION_STATUS: (uuid: "
          << updateOfferOperationStatus->update.operation_uuid()
          << ") for framework "
          << updateOfferOperationStatus->update.framework_id()
          << " (latest state: "
          << updateOfferOperationStatus->update.latest_status().state()
          << ", status update state: "
          << updateOfferOperationStatus->update.status().state() << ")";
    }

    case ResourceProviderMessage::Type::DISCONNECT: {
      const Option<ResourceProviderMessage::Disconnect>& disconnect =
        resourceProviderMessage.disconnect;

      CHECK_SOME(disconnect);

      return stream
          << "DISCONNECT: resource provider "
          << disconnect->resourceProviderId;
    }
  }

  UNREACHABLE();
}

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_MESSAGE_HPP__
