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

#include "resource_provider/manager.hpp"

#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <mesos/http.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/pull_gauge.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/registry.hpp"
#include "resource_provider/validation.hpp"

namespace http = process::http;

using std::string;
using std::vector;

using mesos::internal::resource_provider::validation::call::validate;

using mesos::resource_provider::AdmitResourceProvider;
using mesos::resource_provider::Call;
using mesos::resource_provider::Event;
using mesos::resource_provider::Registrar;
using mesos::resource_provider::RemoveResourceProvider;

using mesos::resource_provider::registry::Registry;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::Queue;

using process::collect;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::OK;
using process::http::Pipe;
using process::http::UnsupportedMediaType;

using process::http::authentication::Principal;

using process::metrics::Counter;
using process::metrics::PullGauge;

namespace mesos {
namespace internal {

mesos::resource_provider::registry::ResourceProvider
createRegistryResourceProvider(const ResourceProviderInfo& resourceProviderInfo)
{
  mesos::resource_provider::registry::ResourceProvider resourceProvider;

  CHECK(resourceProviderInfo.has_id());
  resourceProvider.mutable_id()->CopyFrom(resourceProviderInfo.id());

  resourceProvider.set_name(resourceProviderInfo.name());
  resourceProvider.set_type(resourceProviderInfo.type());

  return resourceProvider;
}


struct ResourceProvider
{
  ResourceProvider(
      const ResourceProviderInfo& _info,
      const StreamingHttpConnection<v1::resource_provider::Event>& _http)
    : info(_info),
      http(_http) {}

  ~ResourceProvider()
  {
    LOG(INFO) << "Terminating resource provider " << info.id();

    http.close();

    foreachvalue (const Owned<Promise<Nothing>>& publish, publishes) {
      publish->fail(
          "Failed to publish resources from resource provider " +
          stringify(info.id()) + ": Connection closed");
    }
  }

  ResourceProviderInfo info;
  StreamingHttpConnection<v1::resource_provider::Event> http;
  hashmap<id::UUID, Owned<Promise<Nothing>>> publishes;
};


class ResourceProviderManagerProcess
  : public Process<ResourceProviderManagerProcess>
{
public:
  ResourceProviderManagerProcess(Owned<Registrar> _registrar);

  Future<http::Response> api(
      const http::Request& request,
      const Option<Principal>& principal);

  void applyOperation(const ApplyOperationMessage& message);

  void acknowledgeOperationStatus(
      const AcknowledgeOperationStatusMessage& message);

  void reconcileOperations(const ReconcileOperationsMessage& message);

  Future<Nothing> removeResourceProvider(
      const ResourceProviderID& resourceProviderId);

  Future<Nothing> publishResources(const Resources& resources);

  Queue<ResourceProviderMessage> messages;

private:
  void subscribe(
      const StreamingHttpConnection<v1::resource_provider::Event>& http,
      const Call::Subscribe& subscribe);

  void _subscribe(
      const Future<bool>& admitResourceProvider,
      Owned<ResourceProvider> resourceProvider);

  void updateOperationStatus(
      ResourceProvider* resourceProvider,
      const Call::UpdateOperationStatus& update);

  ResourceProviderMessage createUpdateOperationStatus(
      const OperationStatus& operationStatus,
      const Option<id::UUID>& operationUuid,
      const Option<FrameworkID>& frameworkId,
      const Option<OperationStatus>& latestStatus);

  void updateState(
      ResourceProvider* resourceProvider,
      const Call::UpdateState& update);

  void updatePublishResourcesStatus(
      ResourceProvider* resourceProvider,
      const Call::UpdatePublishResourcesStatus& update);

  Future<Nothing> recover(
      const mesos::resource_provider::registry::Registry& registry);

  void initialize() override;

  ResourceProviderID newResourceProviderId();

  double gaugeSubscribed();

  struct ResourceProviders
  {
    hashmap<ResourceProviderID, Owned<ResourceProvider>> subscribed;
    hashmap<
        ResourceProviderID,
        mesos::resource_provider::registry::ResourceProvider>
      known;
  } resourceProviders;

  struct Metrics
  {
    explicit Metrics(const ResourceProviderManagerProcess& manager);
    ~Metrics();

    PullGauge subscribed;
    Counter subscriptions;
    Counter disconnections;
  };

  Owned<Registrar> registrar;
  Promise<Nothing> recovered;

  Metrics metrics;
};


ResourceProviderManagerProcess::ResourceProviderManagerProcess(
    Owned<Registrar> _registrar)
  : ProcessBase(process::ID::generate("resource-provider-manager")),
    registrar(std::move(_registrar)),
    metrics(*this)
{
  CHECK_NOTNULL(registrar.get());
}


void ResourceProviderManagerProcess::initialize()
{
  // Recover the registrar.
  registrar->recover()
    .then(defer(self(), &ResourceProviderManagerProcess::recover, lambda::_1))
    .onAny([](const Future<Nothing>& recovered) {
      if (!recovered.isReady()) {
        LOG(FATAL)
        << "Failed to recover resource provider manager registry: "
        << recovered;
      }
    });
}


Future<Nothing> ResourceProviderManagerProcess::recover(
    const mesos::resource_provider::registry::Registry& registry)
{
  foreach (
      const mesos::resource_provider::registry::ResourceProvider&
        resourceProvider,
      registry.resource_providers()) {
    resourceProviders.known.put(resourceProvider.id(), resourceProvider);
  }

  recovered.set(Nothing());

  return Nothing();
}


Future<http::Response> ResourceProviderManagerProcess::api(
    const http::Request& request,
    const Option<Principal>& principal)
{
  // TODO(bbannier): This implementation does not limit the number of messages
  // in the actor's inbox which could become large should a big number of
  // resource providers attempt to subscribe before recovery completed. Consider
  // rejecting requests until the resource provider manager has recovered. This
  // would likely require implementing retry logic in resource providers.
  return recovered.future().then(defer(
      self(), [this, request, principal](const Nothing&) -> http::Response {
        if (request.method != "POST") {
          return MethodNotAllowed({"POST"}, request.method);
        }

        v1::resource_provider::Call v1Call;

        // TODO(anand): Content type values are case-insensitive.
        Option<string> contentType = request.headers.get("Content-Type");

        if (contentType.isNone()) {
          return BadRequest("Expecting 'Content-Type' to be present");
        }

        if (contentType.get() == APPLICATION_PROTOBUF) {
          if (!v1Call.ParseFromString(request.body)) {
            return BadRequest("Failed to parse body into Call protobuf");
          }
        } else if (contentType.get() == APPLICATION_JSON) {
          Try<JSON::Value> value = JSON::parse(request.body);
          if (value.isError()) {
            return BadRequest(
                "Failed to parse body into JSON: " + value.error());
          }

          Try<v1::resource_provider::Call> parse =
            ::protobuf::parse<v1::resource_provider::Call>(value.get());

          if (parse.isError()) {
            return BadRequest(
                "Failed to convert JSON into Call protobuf: " + parse.error());
          }

          v1Call = parse.get();
        } else {
          return UnsupportedMediaType(
              string("Expecting 'Content-Type' of ") + APPLICATION_JSON +
              " or " + APPLICATION_PROTOBUF);
        }

        Call call = devolve(v1Call);

        Option<Error> error = validate(call, None());
        if (error.isSome()) {
          return BadRequest(
              "Failed to validate resource_provider::Call: " + error->message);
        }

        if (call.type() == Call::SUBSCRIBE) {
          // We default to JSON 'Content-Type' in the response since an empty
          // 'Accept' header results in all media types considered acceptable.
          ContentType acceptType = ContentType::JSON;

          if (request.acceptsMediaType(APPLICATION_JSON)) {
            acceptType = ContentType::JSON;
          } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
            acceptType = ContentType::PROTOBUF;
          } else {
            return NotAcceptable(
                string("Expecting 'Accept' to allow ") + "'" +
                APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
          }

          if (request.headers.contains("Mesos-Stream-Id")) {
            return BadRequest(
                "Subscribe calls should not include the 'Mesos-Stream-Id' "
                "header");
          }

          Pipe pipe;
          OK ok;

          ok.headers["Content-Type"] = stringify(acceptType);
          ok.type = http::Response::PIPE;
          ok.reader = pipe.reader();

          // Generate a stream ID and return it in the response.
          id::UUID streamId = id::UUID::random();
          ok.headers["Mesos-Stream-Id"] = streamId.toString();

          StreamingHttpConnection<v1::resource_provider::Event> http(
              pipe.writer(), acceptType, streamId);

          this->subscribe(http, call.subscribe());

          return std::move(ok);
        }

        if (!this->resourceProviders.subscribed.contains(
                call.resource_provider_id())) {
          return BadRequest("Resource provider is not subscribed");
        }

        ResourceProvider* resourceProvider = CHECK_NOTNULL(
            this->resourceProviders.subscribed.at(call.resource_provider_id())
              .get());

        // Perform additional validation with the now known provider info.
        error = validate(call, resourceProvider->info);
        if (error.isSome()) {
          return BadRequest("Inconsistent call: " + error->message);
        }

        // This isn't a `SUBSCRIBE` call, so the request should include a stream
        // ID.
        if (!request.headers.contains("Mesos-Stream-Id")) {
          return BadRequest(
              "All non-subscribe calls should include to 'Mesos-Stream-Id' "
              "header");
        }

        const string& streamId = request.headers.at("Mesos-Stream-Id");
        if (streamId != resourceProvider->http.streamId.toString()) {
          return BadRequest(
              "The stream ID '" + streamId +
              "' included in this request "
              "didn't match the stream ID currently associated with "
              " resource provider ID " +
              resourceProvider->info.id().value());
        }

        switch (call.type()) {
          case Call::UNKNOWN: {
            return NotImplemented();
          }

          case Call::SUBSCRIBE: {
            // `SUBSCRIBE` call should have been handled above.
            LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";
          }

          case Call::UPDATE_OPERATION_STATUS: {
            this->updateOperationStatus(
                resourceProvider, call.update_operation_status());

            return Accepted();
          }

          case Call::UPDATE_STATE: {
            this->updateState(resourceProvider, call.update_state());
            return Accepted();
          }

          case Call::UPDATE_PUBLISH_RESOURCES_STATUS: {
            this->updatePublishResourcesStatus(
                resourceProvider, call.update_publish_resources_status());
            return Accepted();
          }
        }

        UNREACHABLE();
      }));
}


void ResourceProviderManagerProcess::applyOperation(
    const ApplyOperationMessage& message)
{
  const Offer::Operation& operation = message.operation_info();
  const Option<FrameworkID> frameworkId = message.has_framework_id()
    ? message.framework_id()
    : Option<FrameworkID>::none();
  const UUID& operationUUID = message.operation_uuid();

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation);

  if (!resourceProviderId.isSome()) {
    LOG(ERROR) << "Failed to get the resource provider ID of operation "
               << "'" << operation.id() << "' (uuid: " << operationUUID
               << ") from "
               << (frameworkId.isSome()
                     ? "framework " + stringify(frameworkId.get())
                     : "an operator API call")
               << ": "
               << (resourceProviderId.isError() ? resourceProviderId.error()
                                                : "Not found");
    return;
  }

  if (!resourceProviders.subscribed.contains(resourceProviderId.get())) {
    LOG(WARNING) << "Dropping operation '" << operation.id() << "' (uuid: "
                 << operationUUID << ") from "
                 << (frameworkId.isSome()
                       ? "framework " + stringify(frameworkId.get())
                       : "an operator API call")
                 << " because resource provider " << resourceProviderId.get()
                 << " is not subscribed";
    return;
  }

  ResourceProvider* resourceProvider =
    resourceProviders.subscribed.at(resourceProviderId.get()).get();

  CHECK(message.resource_version_uuid().has_resource_provider_id());

  CHECK_EQ(message.resource_version_uuid().resource_provider_id(),
           resourceProviderId.get())
    << "Resource provider ID "
    << message.resource_version_uuid().resource_provider_id()
    << " in resource version UUID does not match that in the operation "
    << resourceProviderId.get();

  Event event;
  event.set_type(Event::APPLY_OPERATION);
  if (frameworkId.isSome()) {
    event.mutable_apply_operation()
      ->mutable_framework_id()->CopyFrom(frameworkId.get());
  }
  event.mutable_apply_operation()->mutable_info()->CopyFrom(operation);
  event.mutable_apply_operation()
    ->mutable_operation_uuid()->CopyFrom(message.operation_uuid());
  event.mutable_apply_operation()
    ->mutable_resource_version_uuid()
    ->CopyFrom(message.resource_version_uuid().uuid());

  if (!resourceProvider->http.send(event)) {
    LOG(WARNING) << "Failed to send operation '" << operation.id() << "' "
                 << "(uuid: " << operationUUID << ") from "
                 << (frameworkId.isSome()
                       ? "framework " + stringify(frameworkId.get())
                       : "an operator API call")
                 << " to resource provider " << resourceProviderId.get()
                 << ": connection closed";
  }
}


void ResourceProviderManagerProcess::acknowledgeOperationStatus(
    const AcknowledgeOperationStatusMessage& message)
{
  CHECK(message.has_resource_provider_id());

  if (!resourceProviders.subscribed.contains(message.resource_provider_id())) {
    LOG(WARNING) << "Dropping operation status acknowledgement with"
                 << " status_uuid " << message.status_uuid() << " and"
                 << " operation_uuid " << message.operation_uuid() << " because"
                 << " resource provider " << message.resource_provider_id()
                 << " is not subscribed";
    return;
  }

  ResourceProvider& resourceProvider =
    *resourceProviders.subscribed.at(message.resource_provider_id());

  Event event;
  event.set_type(Event::ACKNOWLEDGE_OPERATION_STATUS);
  event.mutable_acknowledge_operation_status()
    ->mutable_status_uuid()
    ->CopyFrom(message.status_uuid());
  event.mutable_acknowledge_operation_status()
    ->mutable_operation_uuid()
    ->CopyFrom(message.operation_uuid());

  if (!resourceProvider.http.send(event)) {
    LOG(WARNING) << "Failed to send operation status acknowledgement with"
                 << " status_uuid " << message.status_uuid() << " and"
                 << " operation_uuid " << message.operation_uuid() << " to"
                 << " resource provider " << message.resource_provider_id()
                 << ": connection closed";
  }
}


void ResourceProviderManagerProcess::reconcileOperations(
    const ReconcileOperationsMessage& message)
{
  hashmap<ResourceProviderID, Event> events;

  auto addOperation =
    [&events](const ReconcileOperationsMessage::Operation& operation) {
      const ResourceProviderID resourceProviderId =
        operation.resource_provider_id();

      if (events.contains(resourceProviderId)) {
        events.at(resourceProviderId).mutable_reconcile_operations()
          ->add_operation_uuids()->CopyFrom(operation.operation_uuid());
      } else {
        Event event;
        event.set_type(Event::RECONCILE_OPERATIONS);
        event.mutable_reconcile_operations()
          ->add_operation_uuids()->CopyFrom(operation.operation_uuid());

        events[resourceProviderId] = event;
      }
  };

  // If the framework ID is set, then this reconciliation request originated
  // from the framework; otherwise, it originated from the master. If it's from
  // the framework, then we return an operation state based on whether or not
  // the resource provider is known. If it's from the master, then we forward it
  // to the resource provider to determine whether or not this operation was
  // dropped en route to the agent.
  Option<FrameworkID> frameworkId = message.has_framework_id()
    ? message.framework_id()
    : Option<FrameworkID>::none();

  // Construct events for individual resource providers, or send appropriate
  // operation status updates if resource providers are not subscribed.
  foreach (
      const ReconcileOperationsMessage::Operation& operation,
      message.operations()) {
    if (operation.has_resource_provider_id()) {
      if (!resourceProviders.subscribed.contains(
              operation.resource_provider_id())) {
        OperationState state;
        if (resourceProviders.known.contains(
                operation.resource_provider_id())) {
          // The resource provider is not currently subscribed, but since it is
          // known, we expect it to resubscribe. We send OPERATION_RECOVERING in
          // this case.
          state = OPERATION_RECOVERING;
        } else {
          // The resource provider is not known; we return OPERATION_UNKNOWN in
          // this case.
          state = OPERATION_UNKNOWN;
        }

        Option<id::UUID> uuid;
        if (operation.has_operation_uuid()) {
          Try<id::UUID> uuid_ =
            id::UUID::fromBytes(operation.operation_uuid().value());
          CHECK_SOME(uuid_);
          uuid = uuid_.get();
        }

        // NOTE: the `SlaveID` is left blank in the operation status below
        // because the RP manager does not have access to this information. The
        // agent will inject the ID before forwarding to the master.
        messages.put(
            createUpdateOperationStatus(
                protobuf::createOperationStatus(
                    state,
                    operation.has_operation_id()
                      ? operation.operation_id()
                      : Option<OperationID>::none(),
                    None(),
                    None(),
                    uuid,
                    None(),
                    operation.resource_provider_id()),
                uuid,
                frameworkId,
                None()));

        continue;
      }

      addOperation(operation);
    }
  }

  foreachpair (
      const ResourceProviderID& resourceProviderId,
      const Event& event,
      events) {
    CHECK(resourceProviders.subscribed.contains(resourceProviderId));
    ResourceProvider& resourceProvider =
      *resourceProviders.subscribed.at(resourceProviderId);

    if (!resourceProvider.http.send(event)) {
      LOG(WARNING) << "Failed to send operation reconciliation event"
                   << " to resource provider " << resourceProviderId
                   << ": connection closed";
    }
  }
}


Future<Nothing> ResourceProviderManagerProcess::removeResourceProvider(
    const ResourceProviderID& resourceProviderId)
{
  LOG(INFO) << "Removing resource provider " << resourceProviderId;

  Future<bool> removeResourceProvider = registrar
    ->apply(Owned<mesos::resource_provider::Registrar::Operation>(
        new RemoveResourceProvider(resourceProviderId)));

  removeResourceProvider.onAny(
      [resourceProviderId](const Future<bool>& removeResourceProvider) {
        if (!removeResourceProvider.isReady()) {
          LOG(ERROR) << "Not removing resource provider " << resourceProviderId
                     << " as registry update did not succeed: "
                     << removeResourceProvider;
        }
      });

  return removeResourceProvider
    .then(defer(
        self(),
        [this, resourceProviderId](const Future<bool>& removeResourceProvider) {
          // TODO(bbannier): We should also on a best effort basis send a
          // `TEARDOWN` event when a removed resource provider attempts to
          // resubscribe. This can happen e.g., if the event gets lost on its
          // way to the resource provider, or if the resource provider is not
          // connected.
          if (resourceProviders.subscribed.contains(resourceProviderId)) {
            const Owned<ResourceProvider>& resourceProvider =
              resourceProviders.subscribed.at(resourceProviderId);
            Event event;
            event.set_type(Event::TEARDOWN);

            if (!resourceProvider->http.send(event)) {
              LOG(WARNING)
                << "Failed to send TEARDOWN event to resource provider "
                << resourceProviderId << ": connection closed";
            }
          } else {
            LOG(WARNING)
              << "Failed to send TEARDOWN event to resource provider "
              << resourceProviderId << ": resource provider not subscribed";
          }

          resourceProviders.known.erase(resourceProviderId);
          resourceProviders.subscribed.erase(resourceProviderId);

          ResourceProviderMessage::Remove remove{resourceProviderId};

          ResourceProviderMessage message;
          message.type = ResourceProviderMessage::Type::REMOVE;
          message.remove = std::move(remove);

          messages.put(std::move(message));

          return Nothing();
        }));
}


Future<Nothing> ResourceProviderManagerProcess::publishResources(
    const Resources& resources)
{
  hashmap<ResourceProviderID, Resources> providedResources;

  foreach (const Resource& resource, resources) {
    // NOTE: We ignore agent default resources here because those
    // resources do not need publish, and shouldn't be handled by the
    // resource provider manager.
    if (!resource.has_provider_id()) {
      continue;
    }

    const ResourceProviderID& resourceProviderId = resource.provider_id();

    if (!resourceProviders.subscribed.contains(resourceProviderId)) {
      // TODO(chhsiao): If the manager is running on an agent and the
      // resource comes from an external resource provider, we may want
      // to load the provider's agent component.
      return Failure(
          "Resource provider " + stringify(resourceProviderId) +
          " is not subscribed");
    }

    providedResources[resourceProviderId] += resource;
  }

  vector<Future<Nothing>> futures;

  foreachpair (const ResourceProviderID& resourceProviderId,
               const Resources& resources,
               providedResources) {
    id::UUID uuid = id::UUID::random();

    Event event;
    event.set_type(Event::PUBLISH_RESOURCES);
    *event.mutable_publish_resources()->mutable_uuid() =
      protobuf::createUUID(uuid);
    *event.mutable_publish_resources()->mutable_resources() = resources;

    ResourceProvider* resourceProvider =
      resourceProviders.subscribed.at(resourceProviderId).get();

    LOG(INFO)
      << "Sending PUBLISH event " << uuid << " with resources '" << resources
      << "' to resource provider " << resourceProviderId;

    if (!resourceProvider->http.send(event)) {
      return Failure(
          "Failed to send PUBLISH_RESOURCES event to resource provider " +
          stringify(resourceProviderId) + ": connection closed");
    }

    Owned<Promise<Nothing>> promise(new Promise<Nothing>());
    futures.push_back(promise->future());
    resourceProvider->publishes.put(uuid, std::move(promise));
  }

  return collect(futures).then([] { return Nothing(); });
}


void ResourceProviderManagerProcess::subscribe(
    const StreamingHttpConnection<v1::resource_provider::Event>& http,
    const Call::Subscribe& subscribe)
{
  const ResourceProviderInfo& resourceProviderInfo =
    subscribe.resource_provider_info();

  LOG(INFO) << "Subscribing resource provider " << resourceProviderInfo;

  // We always create a new `ResourceProvider` struct when a
  // resource provider subscribes or resubscribes, and replace the
  // existing `ResourceProvider` if needed.
  Owned<ResourceProvider> resourceProvider(
      new ResourceProvider(resourceProviderInfo, http));

  Future<bool> admitResourceProvider;

  if (!resourceProviderInfo.has_id()) {
    // The resource provider is subscribing for the first time.
    resourceProvider->info.mutable_id()->CopyFrom(newResourceProviderId());

    // If we are handing out a new `ResourceProviderID` persist the ID by
    // triggering a `AdmitResourceProvider` operation on the registrar.
    admitResourceProvider =
      registrar->apply(Owned<mesos::resource_provider::Registrar::Operation>(
          new AdmitResourceProvider(
              createRegistryResourceProvider(resourceProvider->info))));
  } else {
    // TODO(chhsiao): The resource provider is resubscribing after being
    // restarted or an agent failover. The 'ResourceProviderInfo' might
    // have been updated, but its type and name should remain the same.
    // We should checkpoint its 'type', 'name' and ID, then check if the
    // resubscription is consistent with the checkpointed record.

    const ResourceProviderID& resourceProviderId = resourceProviderInfo.id();

    if (!resourceProviders.known.contains(resourceProviderId)) {
      LOG(INFO)
        << "Dropping resubscription attempt of resource provider with ID "
        << resourceProviderId
        << " since it is unknown";

      return;
    }

    // Check whether the resource provider has change
    // information which should be static.
    mesos::resource_provider::registry::ResourceProvider resourceProvider_ =
      createRegistryResourceProvider(resourceProvider->info);

    const mesos::resource_provider::registry::ResourceProvider&
      storedResourceProvider = resourceProviders.known.at(resourceProviderId);

    if (resourceProvider_ != storedResourceProvider) {
      LOG(INFO)
        << "Dropping resubscription attempt of resource provider "
        << resourceProvider_
        << " since it does not match the previous information "
        << storedResourceProvider;
      return;
    }

    // If the resource provider is known we do not need to admit it
    // again, and the registrar operation implicitly succeeded.
    admitResourceProvider = true;
  }

  admitResourceProvider.onAny(defer(
      self(),
      &ResourceProviderManagerProcess::_subscribe,
      lambda::_1,
      std::move(resourceProvider)));
}


void ResourceProviderManagerProcess::_subscribe(
    const Future<bool>& admitResourceProvider,
    Owned<ResourceProvider> resourceProvider)
{
  if (!admitResourceProvider.isReady()) {
    LOG(INFO)
      << "Not subscribing resource provider " << resourceProvider->info.id()
      << " as registry update did not succeed: " << admitResourceProvider;

    return;
  }

  CHECK(admitResourceProvider.get())
    << "Could not admit resource provider " << resourceProvider->info.id()
    << " as registry update was rejected";

  const ResourceProviderID& resourceProviderId = resourceProvider->info.id();

  resourceProvider->http.closed()
    .onAny(defer(self(), [=](const Future<Nothing>& future) {
      // Iff the remote side closes the HTTP connection, the future will be
      // ready. We will remove the resource provider in that case.
      // This side closes the HTTP connection only when removing a resource
      // provider, therefore we shouldn't try to remove it again here.
      if (future.isReady()) {
        CHECK(resourceProviders.subscribed.contains(resourceProviderId));

        // NOTE: All pending futures of publish requests for the resource
        // provider will become failed.
        resourceProviders.subscribed.erase(resourceProviderId);
      }

      ResourceProviderMessage::Disconnect disconnect{resourceProviderId};

      ResourceProviderMessage message;
      message.type = ResourceProviderMessage::Type::DISCONNECT;
      message.disconnect = std::move(disconnect);

      messages.put(std::move(message));

      ++metrics.disconnections;
    }));

  if (!resourceProviders.known.contains(resourceProviderId)) {
    mesos::resource_provider::registry::ResourceProvider resourceProvider_ =
      createRegistryResourceProvider(resourceProvider->info);

    resourceProviders.known.put(
        resourceProviderId,
        std::move(resourceProvider_));
  }

  ResourceProviderMessage::Subscribe subscribe{resourceProvider->info};

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::SUBSCRIBE;
  message.subscribe = std::move(subscribe);

  // Store a pointer to the resource provider so that we can access it
  // even after moving it into the list of subscribed providers below.
  ResourceProvider* _resourceProvider = resourceProvider.get();

  // TODO(jieyu): Start heartbeat for the resource provider.
  resourceProviders.subscribed.put(
      resourceProviderId,
      std::move(resourceProvider));

  messages.put(std::move(message));
  ++metrics.subscriptions;

  // Only send a `SUBSCRIBED` event after we have updated our internal
  // bookkeeping so that e.g., metrics always reflect correct
  // subscription state.
  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_provider_id()
    ->CopyFrom(resourceProviderId);

  if (!_resourceProvider->http.send(event)) {
    LOG(WARNING) << "Failed to send SUBSCRIBED event to resource provider "
                 << resourceProviderId << ": connection closed";
    return;
  }
}


void ResourceProviderManagerProcess::updateOperationStatus(
    ResourceProvider* resourceProvider,
    const Call::UpdateOperationStatus& update)
{
  CHECK_EQ(update.status().resource_provider_id(), resourceProvider->info.id());

  Option<FrameworkID> frameworkId;
  if (update.has_framework_id()) {
    frameworkId = update.framework_id();
  }

  Option<OperationStatus> latestStatus;
  if (update.has_latest_status()) {
    CHECK_EQ(
        update.latest_status().resource_provider_id(),
        resourceProvider->info.id());

    latestStatus = update.latest_status();
  }

  Try<id::UUID> uuid = id::UUID::fromBytes(update.operation_uuid().value());
  CHECK_SOME(uuid);

  messages.put(
      createUpdateOperationStatus(
          update.status(),
          uuid.get(),
          frameworkId,
          latestStatus));
}


ResourceProviderMessage
ResourceProviderManagerProcess::createUpdateOperationStatus(
    const OperationStatus& operationStatus,
    const Option<id::UUID>& operationUuid,
    const Option<FrameworkID>& frameworkId,
    const Option<OperationStatus>& latestStatus)
{
  ResourceProviderMessage::UpdateOperationStatus body;
  body.update.mutable_status()->CopyFrom(operationStatus);

  if (operationUuid.isSome()) {
    body.update.mutable_operation_uuid()->CopyFrom(
        protobuf::createUUID(operationUuid.get()));
  }

  if (frameworkId.isSome()) {
    body.update.mutable_framework_id()->CopyFrom(frameworkId.get());
  }

  if (latestStatus.isSome()) {
    body.update.mutable_latest_status()->CopyFrom(latestStatus.get());
  }

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_OPERATION_STATUS;
  message.updateOperationStatus = std::move(body);

  return message;
}


void ResourceProviderManagerProcess::updateState(
    ResourceProvider* resourceProvider,
    const Call::UpdateState& update)
{
  foreach (const Resource& resource, update.resources()) {
    CHECK_EQ(resource.provider_id(), resourceProvider->info.id());
  }

  // TODO(chhsiao): Report pending operations.

  hashmap<UUID, Operation> operations;
  foreach (const Operation &operation, update.operations()) {
    operations.put(operation.uuid(), operation);
  }

  LOG(INFO)
    << "Received UPDATE_STATE call with resources '" << update.resources()
    << "' and " << operations.size() << " operations from resource provider "
    << resourceProvider->info.id();

  ResourceProviderMessage::UpdateState updateState{
      resourceProvider->info.id(),
      update.resource_version_uuid(),
      update.resources(),
      std::move(operations)};

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_STATE;
  message.updateState = std::move(updateState);

  messages.put(std::move(message));
}


void ResourceProviderManagerProcess::updatePublishResourcesStatus(
    ResourceProvider* resourceProvider,
    const Call::UpdatePublishResourcesStatus& update)
{
  Try<id::UUID> uuid = id::UUID::fromBytes(update.uuid().value());

  if (uuid.isError()) {
    LOG(ERROR)
      << "Ignoring UpdatePublishResourcesStatus from resource provider "
      << resourceProvider->info.id() << ": " << uuid.error();

    return;
  }

  if (!resourceProvider->publishes.contains(uuid.get())) {
    LOG(ERROR)
      << "Ignoring UpdatePublishResourcesStatus from resource provider "
      << resourceProvider->info.id() << ": Unknown UUID " << uuid.get();

    return;
  }

  LOG(INFO)
    << "Received UPDATE_PUBLISH_RESOURCES_STATUS call for PUBLISH_RESOURCES"
    << " event " << uuid.get() << " with " << update.status()
    << " status from resource provider " << resourceProvider->info.id();

  if (update.status() == Call::UpdatePublishResourcesStatus::OK) {
    resourceProvider->publishes.at(uuid.get())->set(Nothing());
  } else {
    // TODO(jieyu): Consider to include an error message in
    // 'UpdatePublishResourcesStatus' and surface that to the caller.
    resourceProvider->publishes.at(uuid.get())->fail(
        "Received " + stringify(update.status()) + " status");
  }

  resourceProvider->publishes.erase(uuid.get());
}


ResourceProviderID ResourceProviderManagerProcess::newResourceProviderId()
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value(id::UUID::random().toString());
  return resourceProviderId;
}


double ResourceProviderManagerProcess::gaugeSubscribed()
{
  return static_cast<double>(resourceProviders.subscribed.size());
}


ResourceProviderManagerProcess::Metrics::Metrics(
    const ResourceProviderManagerProcess& manager)
  : subscribed(
        "resource_provider_manager/subscribed",
        defer(manager, &ResourceProviderManagerProcess::gaugeSubscribed)),
    subscriptions("resource_provider_manager/events/subscribe"),
    disconnections("resource_provider_manager/events/disconnect")
{
  process::metrics::add(subscribed);
  process::metrics::add(subscriptions);
  process::metrics::add(disconnections);
}


ResourceProviderManagerProcess::Metrics::~Metrics()
{
  process::metrics::remove(subscribed);
  process::metrics::remove(subscriptions);
  process::metrics::remove(disconnections);
}


ResourceProviderManager::ResourceProviderManager(Owned<Registrar> registrar)
  : process(new ResourceProviderManagerProcess(std::move(registrar)))
{
  spawn(CHECK_NOTNULL(process.get()));
}


ResourceProviderManager::~ResourceProviderManager()
{
  terminate(process.get());
  wait(process.get());
}


Future<http::Response> ResourceProviderManager::api(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::api,
      request,
      principal);
}


void ResourceProviderManager::applyOperation(
    const ApplyOperationMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::applyOperation,
      message);
}


void ResourceProviderManager::acknowledgeOperationStatus(
    const AcknowledgeOperationStatusMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::acknowledgeOperationStatus,
      message);
}


void ResourceProviderManager::reconcileOperations(
    const ReconcileOperationsMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::reconcileOperations,
      message);
}


Future<Nothing> ResourceProviderManager::removeResourceProvider(
    const ResourceProviderID& resourceProviderId) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::removeResourceProvider,
      resourceProviderId);
}

Future<Nothing> ResourceProviderManager::publishResources(
    const Resources& resources)
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::publishResources,
      resources);
}


Queue<ResourceProviderMessage> ResourceProviderManager::messages() const
{
  return process->messages;
}

} // namespace internal {
} // namespace mesos {
