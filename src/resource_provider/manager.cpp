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

#include <glog/logging.h>

#include <string>
#include <utility>

#include <mesos/http.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/validation.hpp"

namespace http = process::http;

using mesos::internal::resource_provider::validation::call::validate;

using mesos::resource_provider::Call;
using mesos::resource_provider::Event;

using process::Failure;
using process::Future;
using process::Process;
using process::ProcessBase;
using process::Queue;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::OK;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::Pipe;
using process::http::UnsupportedMediaType;

using process::http::authentication::Principal;

using std::string;


namespace mesos {
namespace internal {

// Represents the streaming HTTP connection to a resource provider.
struct HttpConnection
{
  HttpConnection(const http::Pipe::Writer& _writer,
                 ContentType _contentType,
                 UUID _streamId)
    : writer(_writer),
      contentType(_contentType),
      streamId(_streamId),
      encoder(lambda::bind(serialize, contentType, lambda::_1)) {}

  // Converts the message to an Event before sending.
  template <typename Message>
  bool send(const Message& message)
  {
    // We need to evolve the internal 'message' into a
    // 'v1::resource_provider::Event'.
    return writer.write(encoder.encode(evolve(message)));
  }

  bool close()
  {
    return writer.close();
  }

  Future<Nothing> closed() const
  {
    return writer.readerClosed();
  }

  http::Pipe::Writer writer;
  ContentType contentType;
  UUID streamId;
  ::recordio::Encoder<v1::resource_provider::Event> encoder;
};


struct ResourceProvider
{
  ResourceProvider(
      const ResourceProviderInfo& _info,
      const HttpConnection& _http)
    : info(_info),
      http(_http) {}

  ResourceProviderInfo info;
  HttpConnection http;
  Resources resources;
};


class ResourceProviderManagerProcess
  : public Process<ResourceProviderManagerProcess>
{
public:
  ResourceProviderManagerProcess();

  Future<http::Response> api(
      const http::Request& request,
      const Option<Principal>& principal);

  void applyOfferOperation(const ApplyOfferOperationMessage& message);

  Queue<ResourceProviderMessage> messages;

private:
  void subscribe(
      const HttpConnection& http,
      const Call::Subscribe& subscribe);

  void updateOfferOperationStatus(
      ResourceProvider* resourceProvider,
      const Call::UpdateOfferOperationStatus& update);

  void updateState(
      ResourceProvider* resourceProvider,
      const Call::UpdateState& update);

  ResourceProviderID newResourceProviderId();

  struct ResourceProviders
  {
    hashmap<ResourceProviderID, ResourceProvider> subscribed;
  } resourceProviders;
};


ResourceProviderManagerProcess::ResourceProviderManagerProcess()
  : ProcessBase(process::ID::generate("resource-provider-manager"))
{
}


Future<http::Response> ResourceProviderManagerProcess::api(
    const http::Request& request,
    const Option<Principal>& principal)
{
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
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::resource_provider::Call> parse =
      ::protobuf::parse<v1::resource_provider::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  Call call = devolve(v1Call);

  Option<Error> error = validate(call);
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
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }

    if (request.headers.contains("Mesos-Stream-Id")) {
      return BadRequest(
          "Subscribe calls should not include the 'Mesos-Stream-Id' header");
    }

    Pipe pipe;
    OK ok;

    ok.headers["Content-Type"] = stringify(acceptType);
    ok.type = http::Response::PIPE;
    ok.reader = pipe.reader();

    // Generate a stream ID and return it in the response.
    UUID streamId = UUID::random();
    ok.headers["Mesos-Stream-Id"] = streamId.toString();

    HttpConnection http(pipe.writer(), acceptType, streamId);
    subscribe(http, call.subscribe());

    return ok;
  }

  if (!resourceProviders.subscribed.contains(call.resource_provider_id())) {
    return BadRequest("Resource provider is not subscribed");
  }

  ResourceProvider& resourceProvider =
    resourceProviders.subscribed.at(call.resource_provider_id());

  // This isn't a `SUBSCRIBE` call, so the request should include a stream ID.
  if (!request.headers.contains("Mesos-Stream-Id")) {
    return BadRequest(
        "All non-subscribe calls should include to 'Mesos-Stream-Id' header");
  }

  const string& streamId = request.headers.at("Mesos-Stream-Id");
  if (streamId != resourceProvider.http.streamId.toString()) {
    return BadRequest(
        "The stream ID '" + streamId + "' included in this request "
        "didn't match the stream ID currently associated with "
        " resource provider ID " + resourceProvider.info.id().value());
  }

  switch(call.type()) {
    case Call::UNKNOWN: {
      return NotImplemented();
    }

    case Call::SUBSCRIBE: {
      // `SUBSCRIBE` call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";
    }

    case Call::UPDATE_OFFER_OPERATION_STATUS: {
      updateOfferOperationStatus(
          &resourceProvider,
          call.update_offer_operation_status());

      return Accepted();
    }

    case Call::UPDATE_STATE: {
      updateState(&resourceProvider, call.update_state());
      return Accepted();
    }

    case Call::ACKNOWLEDGE_PUBLISH: {
      // TODO(nfnt): Add a 'ACKNOWLEDGE_PUBLISH' handler.
      return NotImplemented();
    }
  }

  UNREACHABLE();
}


void ResourceProviderManagerProcess::applyOfferOperation(
    const ApplyOfferOperationMessage& message)
{
  const Offer::Operation& operation = message.operation_info();
  const FrameworkID& frameworkId = message.framework_id();

  Try<UUID> uuid = UUID::fromBytes(message.operation_uuid());
  if (uuid.isError()) {
    LOG(ERROR) << "Failed to parse offer operation UUID for operation "
               << "'" << operation.id() << "' from framework "
               << frameworkId << ": " << uuid.error();
    return;
  }

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation);

  if (!resourceProviderId.isSome()) {
    LOG(ERROR) << "Failed to get the resource provider ID of operation "
               << "'" << operation.id() << "' (uuid: " << uuid->toString()
               << ") from framework " << frameworkId << ": "
               << (resourceProviderId.isError() ? resourceProviderId.error()
                                                : "Not found");
    return;
  }

  if (!resourceProviders.subscribed.contains(resourceProviderId.get())) {
    LOG(WARNING) << "Dropping operation '" << operation.id() << "' (uuid: "
                 << uuid.get() << ") from framework " << frameworkId
                 << " because resource provider " << resourceProviderId.get()
                 << " is not subscribed";
    return;
  }

  ResourceProvider& resourceProvider =
    resourceProviders.subscribed.at(resourceProviderId.get());

  CHECK(message.resource_version_uuid().has_resource_provider_id());

  CHECK_EQ(message.resource_version_uuid().resource_provider_id(),
           resourceProviderId.get())
    << "Resource provider ID "
    << message.resource_version_uuid().resource_provider_id()
    << " in resource version UUID does not match that in the operation "
    << resourceProviderId.get();

  Event event;
  event.set_type(Event::OPERATION);
  event.mutable_operation()->mutable_framework_id()->CopyFrom(frameworkId);
  event.mutable_operation()->mutable_info()->CopyFrom(operation);
  event.mutable_operation()->set_operation_uuid(message.operation_uuid());
  event.mutable_operation()->set_resource_version_uuid(
      message.resource_version_uuid().uuid());

  if (!resourceProvider.http.send(event)) {
    LOG(WARNING) << "Failed to send operation '" << operation.id() << "' "
                 << "(uuid: " << uuid.get() << ") from framework "
                 << frameworkId << " to resource provider "
                 << resourceProviderId.get() << ": connection closed";
  }
}


void ResourceProviderManagerProcess::subscribe(
    const HttpConnection& http,
    const Call::Subscribe& subscribe)
{
  ResourceProviderInfo resourceProviderInfo =
    subscribe.resource_provider_info();

  LOG(INFO) << "Subscribing resource provider " << resourceProviderInfo;

  if (!resourceProviderInfo.has_id()) {
    // The resource provider is subscribing for the first time.
    resourceProviderInfo.mutable_id()->CopyFrom(newResourceProviderId());

    ResourceProvider resourceProvider(resourceProviderInfo, http);

    Event event;
    event.set_type(Event::SUBSCRIBED);
    event.mutable_subscribed()->mutable_provider_id()->CopyFrom(
        resourceProvider.info.id());

    if (!resourceProvider.http.send(event)) {
      LOG(WARNING) << "Failed to send SUBSCRIBED event to resource provider "
                   << resourceProvider.info.id() << ": connection closed";
    }

    // TODO(jieyu): Start heartbeat for the resource provider.

    resourceProviders.subscribed.put(
        resourceProviderInfo.id(),
        resourceProvider);

    return;
  }

  // TODO(chhsiao): Reject the subscription if it contains an unknown
  // ID or there is already a subscribed instance with the same ID,
  // and add tests for re-subscriptions.
}


void ResourceProviderManagerProcess::updateOfferOperationStatus(
    ResourceProvider* resourceProvider,
    const Call::UpdateOfferOperationStatus& update)
{
  ResourceProviderMessage::UpdateOfferOperationStatus body;
  body.update.mutable_framework_id()->CopyFrom(update.framework_id());
  body.update.mutable_status()->CopyFrom(update.status());
  body.update.set_operation_uuid(update.operation_uuid());
  if (update.has_latest_status()) {
    body.update.mutable_latest_status()->CopyFrom(update.latest_status());
  }

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_OFFER_OPERATION_STATUS;
  message.updateOfferOperationStatus = std::move(body);

  messages.put(std::move(message));
}


void ResourceProviderManagerProcess::updateState(
    ResourceProvider* resourceProvider,
    const Call::UpdateState& update)
{
  Resources resources;

  foreach (const Resource& resource, update.resources()) {
    CHECK_EQ(resource.provider_id(), resourceProvider->info.id());
    resources += resource;
  }

  resourceProvider->resources = std::move(resources);

  // TODO(chhsiao): Report pending operations.

  Try<UUID> resourceVersionUuid =
    UUID::fromBytes(update.resource_version_uuid());

  CHECK_SOME(resourceVersionUuid)
    << "Could not deserialize version of resource provider "
    << resourceProvider->info.id() << ": " << resourceVersionUuid.error();

  ResourceProviderMessage::UpdateTotalResources updateTotalResources{
      resourceProvider->info.id(),
      resourceVersionUuid.get(),
      resourceProvider->resources};

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_TOTAL_RESOURCES;
  message.updateTotalResources = std::move(updateTotalResources);

  messages.put(std::move(message));
}


ResourceProviderID ResourceProviderManagerProcess::newResourceProviderId()
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value(UUID::random().toString());
  return resourceProviderId;
}


ResourceProviderManager::ResourceProviderManager()
  : process(new ResourceProviderManagerProcess())
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


void ResourceProviderManager::applyOfferOperation(
    const ApplyOfferOperationMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::applyOfferOperation,
      message);
}


Queue<ResourceProviderMessage> ResourceProviderManager::messages() const
{
  return process->messages;
}

} // namespace internal {
} // namespace mesos {
