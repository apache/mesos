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

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <sstream>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/master/detector.hpp>

#include <process/async.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/mutex.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/hashmap.hpp>
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/recordio.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "local/local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/validation.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using namespace process;

using std::ostream;
using std::queue;
using std::shared_ptr;
using std::string;
using std::vector;

using mesos::internal::recordio::Reader;

using mesos::master::detector::MasterDetector;

using process::Owned;
using process::wait; // Necessary on some OS's to disambiguate.

using process::http::Connection;
using process::http::Pipe;
using process::http::post;
using process::http::Request;
using process::http::Response;
using process::http::URL;

using ::recordio::Decoder;

namespace mesos {
namespace v1 {
namespace scheduler {

struct Connections
{
  bool operator==(const Connections& that) const
  {
    return subscribe == that.subscribe && nonSubscribe == that.nonSubscribe;
  }

  Connection subscribe; // Used for subscribe call/response.
  Connection nonSubscribe; // Used for all other calls/responses.
};


// The process (below) is responsible for sending/receiving HTTP messages
// to/from the master.
class MesosProcess : public ProtobufProcess<MesosProcess>
{
public:
  MesosProcess(
      const string& master,
      ContentType _contentType,
      const lambda::function<void()>& connected,
      const lambda::function<void()>& disconnected,
      const lambda::function<void(const queue<Event>&)>& received,
      const Option<Credential>& _credential,
      const Option<shared_ptr<MasterDetector>>& _detector)
    : ProcessBase(ID::generate("scheduler")),
      state(DISCONNECTED),
      contentType(_contentType),
      callbacks {connected, disconnected, received},
      credential(_credential),
      local(false)
  {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Load any flags from the environment (we use local::Flags in the
    // event we run in 'local' mode, since it inherits
    // logging::Flags). In the future, just as the TODO in
    // local/main.cpp discusses, we'll probably want a way to load
    // master::Flags and slave::Flags as well.
    local::Flags flags;

    Try<Nothing> load = flags.load("MESOS_");

    if (load.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to load flags: " << load.error();
    }

    // Initialize libprocess (done here since at some point we might
    // want to use flags to initialize libprocess).
    process::initialize();

    if (self().address.ip.isLoopback()) {
      LOG(WARNING) << "\n**************************************************\n"
                   << "Scheduler driver bound to loopback interface!"
                   << " Cannot communicate with remote master(s)."
                   << " You might want to set 'LIBPROCESS_IP' environment"
                   << " variable to use a routable IP address.\n"
                   << "**************************************************";
    }

    // Initialize logging.
    if (flags.initialize_driver_logging) {
      logging::initialize("mesos", flags);
    } else {
      VLOG(1) << "Disabling initialization of GLOG logging";
    }

    LOG(INFO) << "Version: " << MESOS_VERSION;

    // Launch a local cluster if necessary.
    Option<UPID> pid = None();
    if (master == "local") {
      pid = local::launch(flags);
      local = true;
    }

    if (_detector.isNone()) {
      Try<MasterDetector*> create =
        MasterDetector::create(pid.isSome() ? string(pid.get()) : master);

      if (create.isError()) {
        EXIT(EXIT_FAILURE)
          << "Failed to create a master detector: " << create.error();
      }

      detector.reset(create.get());
    } else {
      detector = _detector.get();
    }
  }

  virtual ~MesosProcess()
  {
    disconnect();

    // Check and see if we need to shutdown a local cluster.
    if (local) {
      local::shutdown();
    }

    // Note that we ignore any callbacks that are enqueued.
  }

  void send(const Call& call)
  {
    Option<Error> error = validation::scheduler::call::validate(devolve(call));

    if (error.isSome()) {
      drop(call, error->message);
      return;
    }

    if (call.type() == Call::SUBSCRIBE && state != CONNECTED) {
      // It might be possible that the scheduler is retrying. We drop the
      // request if we have an ongoing subscribe request in flight or if the
      // scheduler is already subscribed.
      drop(call, "Scheduler is in state " + stringify(state));
      return;
    }

    if (call.type() != Call::SUBSCRIBE && state != SUBSCRIBED) {
      // We drop all non-subscribe calls if we are not currently subscribed.
      drop(call, "Scheduler is in state " + stringify(state));
      return;
    }

    VLOG(1) << "Sending " << call.type() << " call to " << master.get();

    // TODO(vinod): Add support for sending MESSAGE calls directly
    // to the slave, instead of relaying it through the master, as
    // the scheduler driver does.

    ::Request request;
    request.method = "POST";
    request.url = master.get();
    request.body = serialize(contentType, call);
    request.keepAlive = true;
    request.headers = {{"Accept", stringify(contentType)},
                       {"Content-Type", stringify(contentType)}};

    // TODO(anand): Add support for other authentication schemes.

    if (credential.isSome()) {
      request.headers["Authorization"] =
        "Basic " +
        base64::encode(credential->principal() + ":" + credential->secret());
    }

    CHECK_SOME(connections);

    Future<Response> response;
    if (call.type() == Call::SUBSCRIBE) {
      state = SUBSCRIBING;

      // Send a streaming request for Subscribe call.
      response = connections->subscribe.send(request, true);
    } else {
      CHECK_SOME(streamId);

      // Set the stream ID associated with this connection.
      request.headers["Mesos-Stream-Id"] = streamId->toString();

      response = connections->nonSubscribe.send(request);
    }

    CHECK_SOME(connectionId);
    response.onAny(defer(self(),
                         &Self::_send,
                         connectionId.get(),
                         call,
                         lambda::_1));
  }

  void reconnect()
  {
    // Ignore the reconnection request if we are currently disconnected
    // from the master.
    if (state == DISCONNECTED) {
      VLOG(1) << "Ignoring reconnect request from scheduler since we are"
              << " disconnected";

      return;
    }

    CHECK_SOME(connectionId);

    disconnected(connectionId.get(),
                 "Received reconnect request from scheduler");
  }

protected:
  virtual void initialize()
  {
    // Start detecting masters.
    detection = detector->detect()
      .onAny(defer(self(), &MesosProcess::detected, lambda::_1));
  }

  void connect()
  {
    CHECK_EQ(DISCONNECTED, state);
    CHECK_SOME(master);

    connectionId = UUID::random();

    state = CONNECTING;

    // These automatic variables are needed for lambda capture. We need to
    // create a copy here because `master` or `connectionId` values might change
    // by the time the second `http::connect` gets called.
    ::URL master_ = master.get();
    UUID connectionId_ = connectionId.get();

    // We create two persistent connections here, one for subscribe
    // call/streaming response and another for non-subscribe calls/responses.
    process::http::connect(master_)
      .onAny(defer(self(), [this, master_, connectionId_](
                               const Future<Connection>& connection) {
        process::http::connect(master_)
          .onAny(defer(self(),
                       &Self::connected,
                       connectionId_,
                       connection,
                       lambda::_1));
      }));
  }

  void connected(
      const UUID& _connectionId,
      const Future<Connection>& connection1,
      const Future<Connection>& connection2)
  {
    // It is possible that a new master was detected while we had an ongoing
    // (re-)connection attempt with the old master.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring connection attempt from stale connection";
      return;
    }

    CHECK_EQ(CONNECTING, state);
    CHECK_SOME(connectionId);

    if (!connection1.isReady()) {
      disconnected(connectionId.get(),
                   connection1.isFailed()
                     ? connection1.failure()
                     : "Subscribe future discarded");
      return;
    }

    if (!connection2.isReady()) {
      disconnected(connectionId.get(),
                   connection2.isFailed()
                     ? connection2.failure()
                     : "Non-subscribe future discarded");
      return;
    }

    VLOG(1) << "Connected with the master at " << master.get();

    state = CONNECTED;

    connections = Connections {connection1.get(), connection2.get()};

    connections->subscribe.disconnected()
      .onAny(defer(self(),
                   &Self::disconnected,
                   connectionId.get(),
                   "Subscribe connection interrupted"));

    connections->nonSubscribe.disconnected()
      .onAny(defer(self(),
                   &Self::disconnected,
                   connectionId.get(),
                   "Non-subscribe connection interrupted"));

    // Invoke the connected callback once we have established both subscribe
    // and non-subscribe connections with the master.
    mutex.lock()
      .then(defer(self(), [this]() {
        return async(callbacks.connected);
      }))
      .onAny(lambda::bind(&Mutex::unlock, mutex));
  }

  void disconnected(
      const UUID& _connectionId,
      const string& failure)
  {
    // Ignore if the disconnection happened from an old stale connection.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring disconnection attempt from stale connection";
      return;
    }

    // We can reach here if we noticed a disconnection for either of
    // subscribe/non-subscribe connections. We discard the future here to
    // trigger a master re-detection.
    detection.discard();
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

    state = DISCONNECTED;

    connections = None();
    connectionId = None();
    subscribed = None();
  }

  void detected(const Future<Option<mesos::MasterInfo>>& future)
  {
    if (future.isFailed()) {
      error("Failed to detect a master: " + future.failure());
      return;
    }

    if (state == CONNECTED || state == SUBSCRIBING || state == SUBSCRIBED) {
      // Invoke the disconnected callback if we were previously connected.
      mutex.lock()
        .then(defer(self(), [this]() {
          return async(callbacks.disconnected);
        }))
      .onAny(lambda::bind(&Mutex::unlock, mutex));
    }

    // Disconnect any active connections.
    disconnect();

    Option<mesos::MasterInfo> latest;
    if (future.isDiscarded()) {
      LOG(INFO) << "Re-detecting master";
      master = None();
      latest = None();
    } else if (future->isNone()) {
      LOG(INFO) << "Lost leading master";
      master = None();
      latest = None();
    } else {
      const UPID& upid = future.get().get().pid();
      latest = future.get();

      string scheme = "http";

#ifdef USE_SSL_SOCKET
      Option<string> value;

      value = os::getenv("SSL_ENABLED");
      if (value.isSome() && (value.get() == "1" || value.get() == "true")) {
        scheme = "https";
      }
#endif

      master = ::URL(
        scheme,
        upid.address.ip,
        upid.address.port,
        upid.id +
        "/api/v1/scheduler");

      LOG(INFO) << "New master detected at " << upid;

      connect();
    }

    // Keep detecting masters.
    detection = detector->detect(latest)
      .onAny(defer(self(), &MesosProcess::detected, lambda::_1));
  }

  Future<Nothing> _receive()
  {
    Future<Nothing> future = async(callbacks.received, events);
    events = queue<Event>();
    return future;
  }

  // Helper for injecting an ERROR event.
  void error(const string& message)
  {
    Event event;

    event.set_type(Event::ERROR);

    Event::Error* error = event.mutable_error();

    error->set_message(message);

    receive(event, true);
  }

  void drop(const Call& call, const string& message)
  {
    LOG(WARNING) << "Dropping " << call.type() << ": " << message;
  }

  void _send(
      const UUID& _connectionId,
      const Call& call,
      const Future<Response>& response)
  {
    // It is possible that we detected a new master before a response could
    // be received.
    if (connectionId != _connectionId) {
      return;
    }

    CHECK(!response.isDiscarded());
    CHECK(state == SUBSCRIBING || state == SUBSCRIBED) << state;

    // This can happen during a master failover or a network blip
    // causing the socket to timeout. Eventually, the scheduler would
    // detect the disconnection via ZK(disconnect()) or lack of heartbeats.
    if (response.isFailed()) {
      LOG(ERROR) << "Request for call type " << call.type() << " failed: "
                 << response.failure();
      return;
    }

    if (response->code == process::http::Status::OK) {
      // Only SUBSCRIBE call should get a "200 OK" response.
      CHECK_EQ(Call::SUBSCRIBE, call.type());
      CHECK_EQ(response->type, http::Response::PIPE);
      CHECK_SOME(response->reader);

      state = SUBSCRIBED;

      Pipe::Reader reader = response->reader.get();

      auto deserializer =
        lambda::bind(deserialize<Event>, contentType, lambda::_1);

      Owned<Reader<Event>> decoder(
          new Reader<Event>(Decoder<Event>(deserializer), reader));

      subscribed = SubscribedResponse {reader, decoder};

      // Responses to SUBSCRIBE calls should always include a stream ID.
      CHECK(response->headers.contains("Mesos-Stream-Id"));

      streamId = UUID::fromString(response->headers.at("Mesos-Stream-Id"));

      read();

      return;
    }

    if (response->code == process::http::Status::ACCEPTED) {
      // Only non SUBSCRIBE calls should get a "202 Accepted" response.
      CHECK_NE(Call::SUBSCRIBE, call.type());
      return;
    }

    // We reset the state to connected if the subscribe call did not
    // succceed (e.g., the master was still recovering). The scheduler can
    // then retry the subscribe call.
    if (call.type() == Call::SUBSCRIBE) {
      state = CONNECTED;
    }

    if (response->code == process::http::Status::SERVICE_UNAVAILABLE) {
      // This could happen if the master hasn't realized it is the leader yet
      // or is still in the process of recovery.
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") for " << call.type();
      return;
    }

    if (response->code == process::http::Status::NOT_FOUND) {
      // This could happen if the master libprocess process has not yet set up
      // HTTP routes.
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") for " << call.type();
      return;
    }

    if (response->code == process::http::Status::TEMPORARY_REDIRECT) {
      // This could happen if the detector detects a new leading master before
      // master itself realizes it (e.g., ZK watch delay).
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") for " << call.type();
      return;
    }

    // We should be able to get here only for AuthN errors which is not
    // yet supported for HTTP frameworks.
    error("Received unexpected '" + response->status + "' (" +
          response->body + ") for " + stringify(call.type()));
  }

  void read()
  {
    subscribed->decoder->read()
      .onAny(defer(self(),
                   &Self::_read,
                   subscribed->reader,
                   lambda::_1));
  }

  void _read(const Pipe::Reader& reader, const Future<Result<Event>>& event)
  {
    CHECK(!event.isDiscarded());

    // Ignore enqueued events from the previous Subscribe call reader.
    if (!subscribed.isSome() || subscribed->reader != reader) {
      VLOG(1) << "Ignoring event from old stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    // This could happen if the master failed over while sending a event.
    if (event.isFailed()) {
      LOG(ERROR) << "Failed to decode the stream of events: "
                 << event.failure();
      disconnected(connectionId.get(), event.failure());
      return;
    }

    // This could happen if the master failed over after sending an event.
    if (!event->isSome()) {
      const string error = "End-Of-File received from master. The master "
                           "closed the event stream";
      LOG(ERROR) << error;

      disconnected(connectionId.get(), error);
      return;
    }

    if (event->isError()) {
      error("Failed to de-serialize event: " + event->error());
    } else {
      receive(event.get().get(), false);
    }

    read();
  }

  void receive(const Event& event, bool isLocallyInjected)
  {
    // Check if we're are no longer subscribed but received an event.
    if (!isLocallyInjected && state != SUBSCRIBED) {
      LOG(WARNING) << "Ignoring " << stringify(event.type())
                   << " event because we're no longer subscribed";
      return;
    }

    if (isLocallyInjected) {
      VLOG(1) << "Enqueuing locally injected event " << stringify(event.type());
    } else {
      VLOG(1) << "Enqueuing event " << stringify(event.type()) << " received"
              << " from " << master.get();
    }

    // Queue up the event and invoke the 'received' callback if this
    // is the first event (between now and when the 'received'
    // callback actually gets invoked more events might get queued).
    events.push(event);

    if (events.size() == 1) {
      mutex.lock()
        .then(defer(self(), &Self::_receive))
        .onAny(lambda::bind(&Mutex::unlock, mutex));
    }
  }

private:
  struct Callbacks
  {
    lambda::function<void(void)> connected;
    lambda::function<void(void)> disconnected;
    lambda::function<void(const queue<Event>&)> received;
  };

  struct SubscribedResponse
  {
    Pipe::Reader reader;
    process::Owned<Reader<Event>> decoder;
  };

  enum State
  {
    DISCONNECTED, // Either of subscribe/non-subscribe connection is broken.
    CONNECTING, // Trying to establish subscribe and non-subscribe connections.
    CONNECTED, // Established subscribe and non-subscribe connections.
    SUBSCRIBING, // Trying to subscribe with the master.
    SUBSCRIBED // Subscribed with the master.
  } state;

  friend ostream& operator<<(ostream& stream, State state)
  {
    switch (state) {
      case DISCONNECTED: return stream << "DISCONNECTED";
      case CONNECTING:   return stream << "CONNECTING";
      case CONNECTED:    return stream << "CONNECTED";
      case SUBSCRIBING:  return stream << "SUBSCRIBING";
      case SUBSCRIBED:   return stream << "SUBSCRIBED";
    }

    UNREACHABLE();
  }

  // There can be multiple simulataneous ongoing (re-)connection attempts with
  // the master (e.g., the master failed over while an attempt was in progress).
  // This helps us in uniquely identifying the current connection instance and
  // ignoring the stale instance.
  Option<UUID> connectionId; // UUID to identify the connection instance.

  Option<Connections> connections;
  Option<SubscribedResponse> subscribed;
  ContentType contentType;
  Callbacks callbacks;
  const Option<Credential> credential;
  Mutex mutex; // Used to serialize the callback invocations.
  bool local; // Whether or not we launched a local cluster.
  shared_ptr<MasterDetector> detector;
  queue<Event> events;
  Option<::URL> master;
  Option<UUID> streamId;

  // Master detection future.
  process::Future<Option<mesos::MasterInfo>> detection;
};


Mesos::Mesos(
    const string& master,
    ContentType contentType,
    const lambda::function<void()>& connected,
    const lambda::function<void()>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received,
    const Option<Credential>& credential,
    const Option<shared_ptr<MasterDetector>>& detector)
{
  process = new MesosProcess(
      master,
      contentType,
      connected,
      disconnected,
      received,
      credential,
      detector);

  spawn(process);
}


Mesos::Mesos(
    const string& master,
    ContentType contentType,
    const lambda::function<void()>& connected,
    const lambda::function<void()>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received,
    const Option<Credential>& credential)
  : Mesos(master,
          contentType,
          connected,
          disconnected,
          received,
          credential,
          None()) {}


Mesos::~Mesos()
{
  if (process != NULL) {
    stop();
  }
}


void Mesos::send(const Call& call)
{
  dispatch(process, &MesosProcess::send, call);
}


void Mesos::reconnect()
{
  dispatch(process, &MesosProcess::reconnect);
}


void Mesos::stop()
{
  if (process != NULL) {
    terminate(process);
    wait(process);

    delete process;
    process = NULL;
  }
}

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {
