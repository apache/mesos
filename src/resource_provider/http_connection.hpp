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

#ifndef __RESOURCE_PROVIDER_HTTP_CONNECTION_HPP__
#define __RESOURCE_PROVIDER_HTTP_CONNECTION_HPP__

#include <glog/logging.h>

#include <functional>
#include <ostream>
#include <string>
#include <tuple>
#include <queue>
#include <utility>

#include <mesos/http.hpp>

#include <mesos/v1/mesos.hpp>

#include <process/async.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/mutex.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>
#include <stout/recordio.hpp>
#include <stout/result.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "resource_provider/detector.hpp"

namespace mesos {
namespace internal {

/**
 * HTTP connection handler.
 *
 * Manages the connection to a Call/Event based v1 API like the
 * resource provider API.
 */
template <typename Call, typename Event>
class HttpConnectionProcess
  : public process::Process<HttpConnectionProcess<Call, Event>>
{
public:
  /**
   * Construct a HTTP connection process.
   *
   * @param prefix prefix of the actor.
   * @param _detector the endpoint detector.
   * @param _contentType the content type expected by this connection.
   * @param validate a callback which will be invoked when a call
   *     needs to be validated.
   * @param connected a callback which will be invoked when the
   *     connection is established.
   * @param disconnected a callback which will be invoked when the
   *     connection is disconnected.
   * @param received a callback which will be be invoked when events
   *     are received.
   */
  HttpConnectionProcess(
      const std::string& prefix,
      process::Owned<EndpointDetector> _detector,
      ContentType _contentType,
      const Option<std::string>& _token,
      const std::function<Option<Error>(const Call&)>& validate,
      const std::function<void(void)>& connected,
      const std::function<void(void)>& disconnected,
      const std::function<void(const std::queue<Event>&)>& received)
    : process::ProcessBase(process::ID::generate(prefix)),
      state(State::DISCONNECTED),
      contentType(_contentType),
      token(_token),
      callbacks {validate, connected, disconnected, received},
      detector(std::move(_detector)) {}

  process::Future<Nothing> send(const Call& call)
  {
    Option<Error> error = callbacks.validate(call);

    if (error.isSome()) {
      return process::Failure(error->message);
    }

    if (endpoint.isNone()) {
      return process::Failure("Not connected to an endpoint");
    }

    if (call.type() == Call::SUBSCRIBE && state != State::CONNECTED) {
      // It might be possible that the client is retrying. We drop the
      // request if we have an ongoing subscribe request in flight or
      // if the client is already subscribed.
      return process::Failure(
          "Cannot process 'SUBSCRIBE' call as the driver is in "
          "state " + stringify(state));
    }

    if (call.type() != Call::SUBSCRIBE && state != State::SUBSCRIBED) {
      // We drop all non-subscribe calls if we are not currently subscribed.
      return process::Failure(
          "Cannot process '" + stringify(call.type()) + "' call "
          "as the driver is in state " + stringify(state));
    }

    CHECK(state == State::CONNECTED || state == State::SUBSCRIBED);
    CHECK_SOME(connections);

    VLOG(1) << "Sending " << call.type() << " call to " << endpoint.get();

    process::http::Request request;
    request.method = "POST";
    request.url = endpoint.get();
    request.body = serialize(contentType, call);
    request.keepAlive = true;
    request.headers = {{"Accept", stringify(contentType)},
                       {"Content-Type", stringify(contentType)}};

    if (token.isSome()) {
      request.headers["Authorization"] = "Bearer " + token.get();
    }

    process::Future<process::http::Response> response;
    if (call.type() == Call::SUBSCRIBE) {
      CHECK_EQ(State::CONNECTED, state);
      state = State::SUBSCRIBING;

      // Send a streaming request for Subscribe call.
      response = connections->subscribe.send(request, true);
    } else {
      if (streamId.isSome()) {
        // Set the stream ID associated with this connection.
        request.headers["Mesos-Stream-Id"] = streamId->toString();
      }

      response = connections->nonSubscribe.send(request);
    }

    CHECK_SOME(connectionId);

    return response.then(
        defer(self(),
              &Self::_send,
              connectionId.get(),
              call,
              lambda::_1));
  }

  void start()
  {
    detection = detector->detect(None())
      .onAny(defer(self(), &Self::detected, lambda::_1));
  }

protected:
  // Because we're deriving from a templated base class, we have
  // to explicitly bring these hidden base class names into scope.
  using process::Process<HttpConnectionProcess<Call, Event>>::self;
  typedef HttpConnectionProcess<Call, Event> Self;

  void finalize() override
  {
    disconnect();
  }

  void detected(const process::Future<Option<process::http::URL>>& future)
  {
    if (future.isFailed()) {
      LOG(WARNING) << "Failed to detect an endpoint: " << future.failure();

      // TODO(nfnt): A non-retryable error might be the reason for the
      // failed future. In that case the client should be informed
      // about this error and the URL dectection aborted.
    }

    // Invoke the disconnected callback if we were previously connected.
    switch (state) {
      case State::CONNECTING:
      case State::DISCONNECTED:
        break;
      case State::CONNECTED:
      case State::SUBSCRIBING:
      case State::SUBSCRIBED: {
        mutex.lock()
          .then(defer(self(), [this]() {
            return process::async(callbacks.disconnected);
          }))
          .onAny(lambda::bind(&process::Mutex::unlock, mutex));
      }
    }

    disconnect();

    if (future.isDiscarded()) {
      LOG(INFO) << "Re-detecting endpoint";

      endpoint = None();
    } else if (future->isNone()) {
      LOG(INFO) << "Lost endpoint";

      endpoint = None();
    } else {
      endpoint = future->get();

      LOG(INFO) << "New endpoint detected at " << endpoint.get();

      connectionId = id::UUID::random();

      dispatch(self(), &Self::connect, connectionId.get());
    }

    detection = detector->detect(endpoint)
      .onAny(defer(self(), &Self::detected, lambda::_1));
  }

  void connect(const id::UUID& _connectionId)
  {
    // It is possible that a new endpoint was detected while we were
    // waiting to establish a connection with the old master.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring connection attempt from stale connection";
      return;
    }

    CHECK_SOME(endpoint);
    CHECK_EQ(State::DISCONNECTED, state);

    state = State::CONNECTING;

    // We create two persistent connections here, one for subscribe
    // call/streaming response and another for non-subscribe
    // calls/responses.
    collect(
        process::http::connect(endpoint.get()),
        process::http::connect(endpoint.get()))
      .onAny(defer(self(), &Self::connected, connectionId.get(), lambda::_1));
  }

  void connected(
      const id::UUID& _connectionId,
      const process::Future<std::tuple<
        process::http::Connection, process::http::Connection>>& _connections)
  {
    // It is possible that a new endpoint was detected while we had an
    // ongoing (re-)connection attempt with the old endpoint.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring connection attempt from stale connection";
      return;
    }

    CHECK_EQ(State::CONNECTING, state);

    if (!_connections.isReady()) {
      disconnected(connectionId.get(),
                   _connections.isFailed()
                     ? _connections.failure()
                     : "Connection future discarded");
      return;
    }

    VLOG(1) << "Connected with the remote endpoint at " << endpoint.get();

    state = State::CONNECTED;

    connections = Connections {
        std::get<0>(_connections.get()),
        std::get<1>(_connections.get())};

    connections->subscribe.disconnected()
      .onAny(defer(
          self(),
          &Self::disconnected,
          connectionId.get(),
          "Subscribe connection interrupted"));

    connections->nonSubscribe.disconnected()
      .onAny(defer(
          self(),
          &Self::disconnected,
          connectionId.get(),
          "Non-subscribe connection interrupted"));

    // Invoke the connected callback once we have established both
    // subscribe and non-subscribe connections with the master.
    mutex.lock()
      .then(defer(self(), [this]() {
        return process::async(callbacks.connected);
      }))
      .onAny(lambda::bind(&process::Mutex::unlock, mutex));
  }

  void disconnect()
  {
    if (connections.isSome()) {
      connections->subscribe.disconnect();
      connections->nonSubscribe.disconnect();
    }

    if (subscribed.isSome()) {
      subscribed->reader.close();
    }

    state = State::DISCONNECTED;

    connections = None();
    subscribed = None();
    endpoint = None();
    connectionId = None();
    detection.discard();
  }

  void disconnected(const id::UUID& _connectionId, const std::string& failure)
  {
    // Ignore if the disconnection happened from an old stale connection.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring disconnection attempt from stale connection";
      return;
    }

    // We can reach here if we noticed a disconnection for either of
    // subscribe/non-subscribe connections. We discard the future here
    // to trigger an endpoint re-detection.
    detection.discard();
  }

  process::Future<Nothing> _send(
      const id::UUID& _connectionId,
      const Call& call,
      const process::http::Response& response)
  {
    // It is possible that we detected a new endpoint before a
    // response could be received.
    if (connectionId != _connectionId) {
      return process::Failure("Ignoring response from stale connection");
    }

    CHECK(state == State::SUBSCRIBING || state == State::SUBSCRIBED) << state;

    if (response.code == process::http::Status::OK) {
      // Only SUBSCRIBE call should get a "200 OK" response.
      CHECK_EQ(Call::SUBSCRIBE, call.type());
      CHECK_EQ(process::http::Response::PIPE, response.type);
      CHECK_SOME(response.reader);

      state = State::SUBSCRIBED;

      process::http::Pipe::Reader reader = response.reader.get();

      process::Owned<recordio::Reader<Event>> decoder(
          new recordio::Reader<Event>(
              lambda::bind(deserialize<Event>, contentType, lambda::_1),
              reader));

      subscribed = SubscribedResponse(reader, std::move(decoder));

      if (response.headers.contains("Mesos-Stream-Id")) {
        Try<id::UUID> uuid =
          id::UUID::fromString(response.headers.at("Mesos-Stream-Id"));

        CHECK_SOME(uuid);

        streamId = uuid.get();
      }

      read();

      return Nothing();
    }

    if (response.code == process::http::Status::ACCEPTED) {
      // Only non SUBSCRIBE calls should get a "202 Accepted" response.
      CHECK_NE(Call::SUBSCRIBE, call.type());
      return Nothing();
    }

    // We reset the state to connected if the subscribe call did not
    // succceed. We can then retry the subscribe call.
    if (call.type() == Call::SUBSCRIBE) {
      state = State::CONNECTED;
    }

    if (response.code == process::http::Status::SERVICE_UNAVAILABLE ||
        response.code == process::http::Status::NOT_FOUND) {
      return process::Failure(
          "Received '" + response.status + "' (" + response.body + ")");
    }

    return process::Failure(
        "Received unexpected '" + response.status +
        "' (" + response.body + ")");
  }

  void read()
  {
    subscribed->decoder->read()
      .onAny(defer(self(),
                   &Self::_read,
                   subscribed->reader,
                   lambda::_1));
  }

  void _read(
      const process::http::Pipe::Reader& reader,
      const process::Future<Result<Event>>& event)
  {
    CHECK(!event.isDiscarded());

    // Ignore enqueued events from the previous Subscribe call reader.
    if (!subscribed.isSome() || subscribed->reader != reader) {
      VLOG(1) << "Ignoring event from old stale connection";
      return;
    }

    CHECK_EQ(State::SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    if (event.isFailed()) {
      LOG(ERROR) << "Failed to decode stream of events: "
                 << event.failure();

      disconnected(connectionId.get(), event.failure());
      return;
    }

    if (event->isNone()) {
      const std::string error = "End-Of-File received";
      LOG(ERROR) << error;

      disconnected(connectionId.get(), error);
      return;
    }

    if (event->isError()) {
      LOG(ERROR) << "Failed to de-serialize event: " << event->error();
    } else {
      receive(event->get());
    }

    read();
  }

  void receive(const Event& event)
  {
    // Check if we're are no longer subscribed but received an event.
    if (state != State::SUBSCRIBED) {
      LOG(WARNING) << "Ignoring " << stringify(event.type())
                   << " event because we're no longer subscribed";
      return;
    }

    // Queue up the event and invoke the 'received' callback if this
    // is the first event (between now and when the 'received'
    // callback actually gets invoked more events might get queued).
    events.push(event);

    if (events.size() == 1) {
      mutex.lock()
        .then(defer(self(), [this]() {
          process::Future<Nothing> future =
            process::async(callbacks.received, events);
          events = std::queue<Event>();
          return future;
        }))
        .onAny(lambda::bind(&process::Mutex::unlock, mutex));
    }
  }

private:
  struct Callbacks
  {
    std::function<Option<Error>(const Call&)> validate;
    std::function<void(void)> connected;
    std::function<void(void)> disconnected;
    std::function<void(const std::queue<Event>&)> received;
  };

  struct Connections
  {
    process::http::Connection subscribe;
    process::http::Connection nonSubscribe;
  };

  struct SubscribedResponse
  {
    SubscribedResponse(
        process::http::Pipe::Reader _reader,
        process::Owned<recordio::Reader<Event>> _decoder)
      : reader(std::move(_reader)),
        decoder(std::move(_decoder)) {}

    // The decoder cannot be copied meaningfully, see MESOS-5122.
    SubscribedResponse(const SubscribedResponse&) = delete;
    SubscribedResponse& operator=(const SubscribedResponse&) = delete;
    SubscribedResponse& operator=(SubscribedResponse&&) = default;
    SubscribedResponse(SubscribedResponse&&) = default;

    process::http::Pipe::Reader reader;
    process::Owned<recordio::Reader<Event>> decoder;
  };

  enum class State
  {
    DISCONNECTED, // Either of subscribe/non-subscribe connection is broken.
    CONNECTING, // Trying to establish subscribe and non-subscribe connections.
    CONNECTED, // Established subscribe and non-subscribe connections.
    SUBSCRIBING, // Trying to subscribe with the remote endpoint.
    SUBSCRIBED // Subscribed with the remote endpoint.
  };

  friend std::ostream& operator<<(std::ostream& stream, State state)
  {
    switch (state) {
      case State::DISCONNECTED: return stream << "DISCONNECTED";
      case State::CONNECTING:   return stream << "CONNECTING";
      case State::CONNECTED:    return stream << "CONNECTED";
      case State::SUBSCRIBING:  return stream << "SUBSCRIBING";
      case State::SUBSCRIBED:   return stream << "SUBSCRIBED";
    }

    UNREACHABLE();
  }

  State state;
  Option<Connections> connections;
  Option<SubscribedResponse> subscribed;
  Option<process::http::URL> endpoint;
  const mesos::ContentType contentType;
  Option<std::string> token;
  const Callbacks callbacks;
  process::Mutex mutex; // Used to serialize the callback invocations.
  process::Owned<EndpointDetector> detector;
  std::queue<Event> events;

  // There can be multiple simulataneous ongoing (re-)connection
  // attempts with the remote endpoint (e.g., the endpoint failed over
  // while an attempt was in progress). This helps us in uniquely
  // identifying the current connection instance and ignoring the
  // stale instance.
  Option<id::UUID> connectionId;
  Option<id::UUID> streamId;

  process::Future<Option<process::http::URL>> detection;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_HTTP_CONNECTION_HPP__
