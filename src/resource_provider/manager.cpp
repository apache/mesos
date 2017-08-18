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

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/validation.hpp"

namespace http = process::http;

using mesos::internal::resource_provider::validation::call::validate;

using mesos::resource_provider::Call;
using mesos::resource_provider::Event;

using process::Failure;
using process::Future;
using process::Owned;
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
      const HttpConnection& _http,
      const Resources& _resources)
    : info(_info),
      http(_http),
      resources(_resources) {}

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

  Queue<ResourceProviderMessage> messages;

  hashmap<ResourceProviderID, ResourceProvider> resourceProviders;

private:
  void subscribe(
      const HttpConnection& http,
      const Call::Subscribe& subscribe);

  void update(
      ResourceProvider* resourceProvider,
      const Call::Update& update);

  ResourceProviderID newResourceProviderId();
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

  if (!resourceProviders.contains(call.resource_provider_id())) {
    return BadRequest("Resource provider cannot be found");
  }

  auto resourceProvider = resourceProviders.at(call.resource_provider_id());

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

    case Call::UPDATE: {
      update(&resourceProvider, call.update());
      return Accepted();
    }
  }

  UNREACHABLE();
}


void ResourceProviderManagerProcess::subscribe(
    const HttpConnection& http,
    const Call::Subscribe& subscribe)
{
  ResourceProviderInfo resourceProviderInfo =
    subscribe.resource_provider_info();
  resourceProviderInfo.mutable_id()->CopyFrom(newResourceProviderId());

  // Inject the `ResourceProviderID` for all subscribed resources.
  Resources resources;
  foreach (Resource resource, subscribe.resources()) {
    resource.mutable_provider_id()->CopyFrom(resourceProviderInfo.id());
    resources += resource;
  }

  ResourceProvider resourceProvider(resourceProviderInfo, http, resources);

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_provider_id()->CopyFrom(
      resourceProvider.info.id());

  if (!resourceProvider.http.send(event)) {
    LOG(WARNING) << "Unable to send event to resource provider "
                 << stringify(resourceProvider.info.id())
                 << ": connection closed";
  }

  resourceProviders.put(resourceProviderInfo.id(), std::move(resourceProvider));
}


void ResourceProviderManagerProcess::update(
    ResourceProvider* resourceProvider,
    const Call::Update& update)
{
  // TODO(nfnt): Implement the 'UPDATE' call handler.
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


Queue<ResourceProviderMessage> ResourceProviderManager::messages() const
{
  return process->messages;
}

} // namespace internal {
} // namespace mesos {
