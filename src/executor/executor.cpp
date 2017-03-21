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

#include <queue>
#include <string>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>

#include <process/async.hpp>
#include <process/clock.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/mutex.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/ssl/flags.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "slave/validation.hpp"

#include "version/version.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::ostream;
using std::queue;
using std::string;

using mesos::internal::recordio::Reader;

using mesos::internal::slave::validation::executor::call::validate;

using process::async;
using process::Clock;
using process::delay;
using process::dispatch;
using process::Future;
using process::Mutex;
using process::Owned;
using process::Timer;

using process::ID::generate;

using process::http::Connection;
using process::http::Headers;
using process::http::Pipe;
using process::http::post;
using process::http::Request;
using process::http::Response;
using process::http::URL;

using process::UPID;

using ::recordio::Decoder;

namespace mesos {
namespace v1 {
namespace executor {

class ShutdownProcess : public process::Process<ShutdownProcess>
{
public:
  explicit ShutdownProcess(const Duration& _gracePeriod)
    : ProcessBase(generate("__shutdown_executor__")),
      gracePeriod(_gracePeriod) {}

protected:
  virtual void initialize()
  {
    VLOG(1) << "Scheduling shutdown of the executor in " << gracePeriod;

    delay(gracePeriod, self(), &Self::kill);
  }

  void kill()
  {
    VLOG(1) << "Committing suicide by killing the process group";

#ifndef __WINDOWS__
    // TODO(vinod): Invoke killtree without killing ourselves.
    // Kill the process group (including ourself).
    killpg(0, SIGKILL);
#else
    LOG(WARNING) << "Shutting down process group. Windows does not support "
      "`killpg`, so we simply call `exit` on the assumption "
      "that the process was generated with the "
      "`MesosContainerizer`, which uses the 'close on exit' "
      "feature of job objects to make sure all child processes "
      "are killed when a parent process exits";
    exit(0);
#endif // __WINDOWS__
    // The signal might not get delivered immediately, so sleep for a
    // few seconds. Worst case scenario, exit abnormally.
    os::sleep(Seconds(5));
    exit(-1);
  }

private:
  const Duration gracePeriod;
};


struct Connections
{
  bool operator==(const Connections& that) const
  {
    return subscribe == that.subscribe && nonSubscribe == that.nonSubscribe;
  }

  Connection subscribe; // Used for subscribe call/response.
  Connection nonSubscribe; // Used for all other calls/responses.
};


// The process (below) is responsible for receiving messages (via events)
// from the agent and sending messages (via calls) to the agent.
class MesosProcess : public ProtobufProcess<MesosProcess>
{
public:
  MesosProcess(
      ContentType _contentType,
      const lambda::function<void(void)>& connected,
      const lambda::function<void(void)>& disconnected,
      const lambda::function<void(const queue<Event>&)>& received)
    : ProcessBase(generate("executor")),
      state(DISCONNECTED),
      contentType(_contentType),
      callbacks {connected, disconnected, received}
  {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Load any logging flags from the environment.
    logging::Flags flags;

    Try<flags::Warnings> load = flags.load("MESOS_");

    if (load.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to load flags: " << load.error();
    }

    // Initialize libprocess.
    process::initialize();

    // Initialize logging.
    if (flags.initialize_driver_logging) {
      logging::initialize("mesos", flags);
    } else {
      VLOG(1) << "Disabling initialization of GLOG logging";
    }

    // Log any flag warnings (after logging is initialized).
    foreach (const flags::Warning& warning, load->warnings) {
      LOG(WARNING) << warning.message;
    }

    LOG(INFO) << "Version: " << MESOS_VERSION;

    spawn(new VersionProcess(), true);

    // Check if this is local (for example, for testing).
    local = os::getenv("MESOS_LOCAL").isSome();

    Option<string> value;

    // Get agent PID from environment.
    value = os::getenv("MESOS_SLAVE_PID");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_SLAVE_PID' to be set in the environment";
    }

    UPID upid(value.get());
    CHECK(upid) << "Failed to parse MESOS_SLAVE_PID '" << value.get() << "'";

    string scheme = "http";

#ifdef USE_SSL_SOCKET
    if (process::network::openssl::flags().enabled) {
      scheme = "https";
    }
#endif // USE_SSL_SOCKET

    agent = ::URL(
        scheme,
        upid.address.ip,
        upid.address.port,
        upid.id +
        "/api/v1/executor");

    // Get checkpointing status from environment.
    value = os::getenv("MESOS_CHECKPOINT");
    checkpoint = value.isSome() && value.get() == "1";

    if (checkpoint) {
      // Get recovery timeout from environment.
      value = os::getenv("MESOS_RECOVERY_TIMEOUT");
      if (value.isSome()) {
        Try<Duration> _recoveryTimeout = Duration::parse(value.get());

        CHECK_SOME(_recoveryTimeout)
            << "Failed to parse MESOS_RECOVERY_TIMEOUT '" << value.get()
            << "': " << _recoveryTimeout.error();

        recoveryTimeout = _recoveryTimeout.get();
      } else {
        EXIT(EXIT_FAILURE)
          << "Expecting 'MESOS_RECOVERY_TIMEOUT' to be set in the environment";
      }

      // Get maximum backoff factor from environment.
      value = os::getenv("MESOS_SUBSCRIPTION_BACKOFF_MAX");
      if (value.isSome()) {
        Try<Duration> _maxBackoff = Duration::parse(value.get());

        CHECK_SOME(_maxBackoff)
            << "Failed to parse MESOS_SUBSCRIPTION_BACKOFF_MAX '"
            << value.get() << "': " << _maxBackoff.error();

        maxBackoff = _maxBackoff.get();
      } else {
        EXIT(EXIT_FAILURE)
          << "Expecting 'MESOS_SUBSCRIPTION_BACKOFF_MAX' to be set"
          << " in the environment";
      }
    }

    // Get executor shutdown grace period from the environment.
    value = os::getenv("MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD");
    if (value.isSome()) {
      Try<Duration> _shutdownGracePeriod = Duration::parse(value.get());

      CHECK_SOME(_shutdownGracePeriod)
          << "Failed to parse MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD '"
          << value.get() << "': " << _shutdownGracePeriod.error();

      shutdownGracePeriod = _shutdownGracePeriod.get();
    } else {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD' to be set"
        << " in the environment";
    }
  }

  void send(const Call& call)
  {
    Option<Error> error = validate(devolve(call));
    if (error.isSome()) {
      drop(call, error->message);
      return;
    }

    if (call.type() == Call::SUBSCRIBE && state != CONNECTED) {
      // It might be possible that the executor is retrying. We drop the
      // request if we have an ongoing subscribe request in flight or if the
      // executor is already subscribed.
      drop(call, "Executor is in state " + stringify(state));
      return;
    }

    if (call.type() != Call::SUBSCRIBE && state != SUBSCRIBED) {
      // We drop all non-subscribe calls if we are not currently subscribed.
      drop(call, "Executor is in state " + stringify(state));
      return;
    }

    VLOG(1) << "Sending " << call.type() << " call to " << agent;

    ::Request request;
    request.method = "POST";
    request.url = agent;
    request.body = serialize(contentType, call);
    request.keepAlive = true;
    request.headers = {{"Accept", stringify(contentType)},
                       {"Content-Type", stringify(contentType)}};

    CHECK_SOME(connections);

    Future<Response> response;
    if (call.type() == Call::SUBSCRIBE) {
      state = SUBSCRIBING;

      // Send a streaming request for Subscribe call.
      response = connections->subscribe.send(request, true);
    } else {
      response = connections->nonSubscribe.send(request);
    }

    CHECK_SOME(connectionId);
    response.onAny(defer(self(),
                         &Self::_send,
                         connectionId.get(),
                         call,
                         lambda::_1));
  }

  ~MesosProcess()
  {
    disconnect();
  }

protected:
  virtual void initialize()
  {
    connect();
  }

  void connect()
  {
    CHECK(state == DISCONNECTED || state == CONNECTING) << state;

    connectionId = UUID::random();

    state = CONNECTING;

    // This automatic variable is needed for lambda capture. We need to
    // create a copy here because `connectionId` might change by the time the
    // second `http::connect()` gets called.
    UUID connectionId_ = connectionId.get();

    // We create two persistent connections here, one for subscribe
    // call/streaming response and another for non-subscribe calls/responses.
    process::http::connect(agent)
      .onAny(defer(self(), [this, connectionId_](
                               const Future<Connection>& connection) {
        process::http::connect(agent)
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
    // It is possible that the agent process failed while we have an ongoing
    // (re-)connection attempt with the agent.
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

    VLOG(1) << "Connected with the agent";

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

    // Cancel the recovery timer if we connected after a disconnection with the
    // agent when framework checkpointing is enabled. This ensures that we have
    // only one active timer instance at a given point of time.
    if (recoveryTimer.isSome()) {
      CHECK(checkpoint);

      Clock::cancel(recoveryTimer.get());
      recoveryTimer = None();
    }

    // Invoke the connected callback once we have established both subscribe
    // and non-subscribe connections with the agent.
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

    CHECK_NE(DISCONNECTED, state);

    VLOG(1) << "Disconnected from agent: " << failure;

    bool connected =
      (state == CONNECTED || state == SUBSCRIBING || state == SUBSCRIBED);

    if (connected) {
      // Invoke the disconnected callback the first time we disconnect from
      // the agent.
      mutex.lock()
        .then(defer(self(), [this]() {
          return async(callbacks.disconnected);
        }))
        .onAny(lambda::bind(&Mutex::unlock, mutex));
    }

    // Disconnect any active connections.
    disconnect();

    // This represents a disconnection due to a backoff attempt after being
    // already disconnected from the agent. We had already started the
    // recovery timer when we initially noticed the disconnection.
    if (recoveryTimer.isSome()) {
      CHECK(checkpoint);

      return;
    }

    if (checkpoint && connected) {
      CHECK_SOME(recoveryTimeout);
      CHECK_NONE(recoveryTimer);

      // Set up the recovery timeout upon disconnection. We only set it once per
      // disconnection. This ensures that when we try to (re-)connect with
      // the agent and are unsuccessful, we don't restart the recovery timer.
      recoveryTimer = delay(
          recoveryTimeout.get(),
          self(),
          &Self::_recoveryTimeout);

      // Backoff and reconnect only if framework checkpointing is enabled.
      backoff();
    } else {
      shutdown();
    }
  }

  void backoff()
  {
    if (state == CONNECTED || state == SUBSCRIBING || state == SUBSCRIBED) {
      return;
    }

    CHECK(state == DISCONNECTED || state == CONNECTING) << state;

    CHECK(checkpoint);
    CHECK_SOME(maxBackoff);

    // Linearly backoff by picking a random duration between 0 and
    // `maxBackoff`.
    Duration backoff = maxBackoff.get() * ((double) os::random() / RAND_MAX);

    VLOG(1) << "Will retry connecting with the agent again in " << backoff;

    connect();

    delay(backoff, self(), &Self::backoff);
  }

  Future<Nothing> _receive()
  {
    Future<Nothing> future = async(callbacks.received, events);
    events = queue<Event>();
    return future;
  }

  void _recoveryTimeout()
  {
    // It's possible that a new connection was established since the timeout
    // fired and we were unable to cancel this timeout. If this occurs, don't
    // bother trying to shutdown the executor.
    if (recoveryTimer.isNone() || !recoveryTimer->timeout().expired()) {
      return;
    }

    CHECK(state == DISCONNECTED || state == CONNECTING) << state;

    CHECK_SOME(recoveryTimeout);
    LOG(INFO) << "Recovery timeout of " << recoveryTimeout.get()
              << " exceeded; Shutting down";

    shutdown();
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

  // Helper for injecting an ERROR event.
  void error(const string& message)
  {
    Event event;
    event.set_type(Event::ERROR);

    Event::Error* error = event.mutable_error();
    error->set_message(message);

    receive(event, true);
  }

  void _send(
      const UUID& _connectionId,
      const Call& call,
      const Future<Response>& response)
  {
    // It is possible that the agent process failed before a response could
    // be received.
    if (connectionId != _connectionId) {
      return;
    }

    CHECK(!response.isDiscarded());
    CHECK(state == SUBSCRIBING || state == SUBSCRIBED) << state;

    // This can happen if the agent process is restarted or a network blip
    // caused the socket to timeout. Eventually, the executor would
    // detect the socket disconnection via the disconnected callback.
    if (response.isFailed()) {
      LOG(ERROR) << "Request for call type " << call.type() << " failed: "
                 << response.failure();
      return;
    }

    if (response->code == process::http::Status::OK) {
      // Only SUBSCRIBE call should get a "200 OK" response.
      CHECK_EQ(Call::SUBSCRIBE, call.type());
      CHECK_EQ(response->type, Response::PIPE);
      CHECK_SOME(response->reader);

      state = SUBSCRIBED;

      Pipe::Reader reader = response->reader.get();

      auto deserializer =
        lambda::bind(deserialize<Event>, contentType, lambda::_1);

      Owned<Reader<Event>> decoder(
          new Reader<Event>(Decoder<Event>(deserializer), reader));

      subscribed = SubscribedResponse {reader, decoder};

      read();
      return;
    }

    if (response->code == process::http::Status::ACCEPTED) {
      // Only non SUBSCRIBE calls should get a "202 Accepted" response.
      CHECK_NE(Call::SUBSCRIBE, call.type());
      return;
    }

    // We reset the state to connected if the subscribe call did not
    // succceed (e.g., the agent has not yet set up HTTP routes). The executor
    // can then retry the subscribe call.
    if (call.type() == Call::SUBSCRIBE) {
      state = CONNECTED;
    }

    if (response->code == process::http::Status::SERVICE_UNAVAILABLE) {
      // This could happen if the agent is still in the process of recovery.
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") for " << call.type();
      return;
    }

    if (response->code == process::http::Status::NOT_FOUND) {
      // This could happen if the agent libprocess process has not yet set up
      // HTTP routes.
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") for " << call.type();
      return;
    }

    // We should not be able to get here since we already do validation
    // of calls before sending them to the agent.
    error("Received unexpected '" + response->status + "' (" +
          response->body + ") for " + stringify(call.type()));
  }

  void read()
  {
    CHECK_SOME(subscribed);

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
    if (subscribed.isNone() || subscribed->reader != reader) {
      VLOG(1) << "Ignoring event from old stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    // This could happen if the agent process died while sending a response.
    if (event.isFailed()) {
      LOG(ERROR) << "Failed to decode the stream of events: "
                 << event.failure();

      disconnected(connectionId.get(), event.failure());
      return;
    }

    // This could happen if the agent failed over after sending an event.
    if (event->isNone()) {
      const string error =  "End-Of-File received from agent. The agent closed "
                            "the event stream";
      LOG(ERROR) << error;

      disconnected(connectionId.get(), error);
      return;
    }

    if (event->isError()) {
      error("Failed to de-serialize event: " + event->error());
      return;
    }

    receive(event.get().get(), false);
    read();
  }

  void receive(const Event& event, bool isLocallyInjected)
  {
    // Check if we're no longer subscribed but received an event.
    if (!isLocallyInjected && state != SUBSCRIBED) {
      LOG(WARNING) << "Ignoring " << stringify(event.type())
                   << " event because we're no longer subscribed";
      return;
    }

    if (isLocallyInjected) {
      VLOG(1) << "Enqueuing locally injected event " << stringify(event.type());
    } else {
      VLOG(1) << "Enqueuing event " << stringify(event.type()) << " received"
              << " from " << agent;
    }

    // Queue up the event and invoke the `received` callback if this
    // is the first event (between now and when the `received`
    // callback actually gets invoked more events might get queued).
    events.push(event);

    if (events.size() == 1) {
      mutex.lock()
        .then(defer(self(), &Self::_receive))
        .onAny(lambda::bind(&Mutex::unlock, mutex));
    }

    if (event.type() == Event::SHUTDOWN) {
      _shutdown();
    }
  }

  void shutdown()
  {
    Event event;
    event.set_type(Event::SHUTDOWN);

    receive(event, true);
  }

  void _shutdown()
  {
    if (!local) {
      spawn(new ShutdownProcess(shutdownGracePeriod), true);
    } else {
      // Process any pending received events from agent and then terminate.
      terminate(this, false);
    }
  }

  void drop(const Call& call, const string& message)
  {
    LOG(WARNING) << "Dropping " << call.type() << ": " << message;
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
    SUBSCRIBING, // Trying to subscribe with the agent.
    SUBSCRIBED // Subscribed with the agent.
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
  // the agent (e.g., the agent process restarted while an attempt was in
  // progress). This helps us in uniquely identifying the current connection
  // instance and ignoring the stale instance.
  Option<UUID> connectionId; // UUID to identify the connection instance.

  ContentType contentType;
  Callbacks callbacks;
  Mutex mutex; // Used to serialize the callback invocations.
  queue<Event> events;
  bool local;
  Option<Connections> connections;
  Option<SubscribedResponse> subscribed;
  ::URL agent;
  bool checkpoint;
  Option<Duration> recoveryTimeout;
  Option<Duration> maxBackoff;
  Option<Timer> recoveryTimer;
  Duration shutdownGracePeriod;
};


Mesos::Mesos(
    ContentType contentType,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
  : process(new MesosProcess(contentType, connected, disconnected, received))
{
  spawn(process.get());
}


Mesos::~Mesos()
{
  terminate(process.get());
  wait(process.get());
}


void Mesos::send(const Call& call)
{
  dispatch(process.get(), &MesosProcess::send, call);
}

} // namespace executor {
} // namespace v1 {
} // namespace mesos {
