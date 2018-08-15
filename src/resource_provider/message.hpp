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
    SUBSCRIBE,
    UPDATE_STATE,
    UPDATE_OPERATION_STATUS,
    DISCONNECT,
    REMOVE
  };

  friend std::ostream& operator<<(std::ostream& stream, const Type& type) {
    switch (type) {
      case Type::SUBSCRIBE:
        return stream << "SUBSCRIBE";
      case Type::UPDATE_STATE:
        return stream << "UPDATE_STATE";
      case Type::UPDATE_OPERATION_STATUS:
        return stream << "UPDATE_OPERATION_STATUS";
      case Type::DISCONNECT:
        return stream << "DISCONNECT";
      case Type::REMOVE:
        return stream << "REMOVE";
    }

    UNREACHABLE();
  }

  struct Subscribe
  {
    ResourceProviderInfo info;
  };

  struct UpdateState
  {
    ResourceProviderID resourceProviderId;
    UUID resourceVersion;
    Resources totalResources;
    hashmap<UUID, Operation> operations;
  };

  struct UpdateOperationStatus
  {
    UpdateOperationStatusMessage update;
  };

  struct Disconnect
  {
    ResourceProviderID resourceProviderId;
  };

  struct Remove
  {
    ResourceProviderID resourceProviderId;
  };

  Type type;

  Option<Subscribe> subscribe;
  Option<UpdateState> updateState;
  Option<UpdateOperationStatus> updateOperationStatus;
  Option<Disconnect> disconnect;
  Option<Remove> remove;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const ResourceProviderMessage& resourceProviderMessage)
{
  stream << stringify(resourceProviderMessage.type) << ": ";

  switch (resourceProviderMessage.type) {
    case ResourceProviderMessage::Type::SUBSCRIBE: {
      const Option<ResourceProviderMessage::Subscribe>& subscribe =
        resourceProviderMessage.subscribe;

      CHECK_SOME(subscribe);

      return stream << subscribe->info;
    }
    case ResourceProviderMessage::Type::UPDATE_STATE: {
      const Option<ResourceProviderMessage::UpdateState>&
        updateState = resourceProviderMessage.updateState;

      CHECK_SOME(updateState);

      return stream
          << updateState->resourceProviderId << " "
          << updateState->totalResources;
    }

    case ResourceProviderMessage::Type::UPDATE_OPERATION_STATUS: {
      const Option<ResourceProviderMessage::UpdateOperationStatus>&
        updateOperationStatus =
          resourceProviderMessage.updateOperationStatus;

      CHECK_SOME(updateOperationStatus);

      return stream
          << "(uuid: "
          << updateOperationStatus->update.operation_uuid()
          << ") for framework "
          << updateOperationStatus->update.framework_id()
          << " (latest state: "
          << updateOperationStatus->update.latest_status().state()
          << ", status update state: "
          << updateOperationStatus->update.status().state() << ")";
    }

    case ResourceProviderMessage::Type::DISCONNECT: {
      const Option<ResourceProviderMessage::Disconnect>& disconnect =
        resourceProviderMessage.disconnect;

      CHECK_SOME(disconnect);

      return stream
          << "resource provider "
          << disconnect->resourceProviderId;
    }

    case ResourceProviderMessage::Type::REMOVE: {
      const Option<ResourceProviderMessage::Remove>& remove =
        resourceProviderMessage.remove;

      CHECK_SOME(remove);

      return stream
          << "resource provider "
          << remove->resourceProviderId;
    }
  }

  UNREACHABLE();
}

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_MESSAGE_HPP__
