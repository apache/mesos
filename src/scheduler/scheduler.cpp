/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <iostream>
#include <queue>
#include <string>
#include <sstream>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/async.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
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
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "local/local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/detector.hpp"
#include "master/validation.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;

using namespace process;

using std::queue;
using std::string;
using std::vector;

using mesos::internal::recordio::Reader;

using process::Owned;
using process::wait; // Necessary on some OS's to disambiguate.

using process::http::Pipe;
using process::http::post;
using process::http::Response;

using ::recordio::Decoder;

namespace mesos {
namespace v1 {
namespace scheduler {

// The process (below) is responsible for receiving messages
// (eventually events) from the master and sending messages (via
// calls) to the master.
class MesosProcess : public ProtobufProcess<MesosProcess>
{
public:
  MesosProcess(
      const string& master,
      ContentType _contentType,
      const lambda::function<void(void)>& _connected,
      const lambda::function<void(void)>& _disconnected,
      lambda::function<void(const queue<Event>&)> _received)
    : ProcessBase(ID::generate("scheduler")),
      contentType(_contentType),
      connected(_connected),
      disconnected(_disconnected),
      received(_received),
      local(false),
      detector(NULL)
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
      error("Failed to load flags: " + load.error());
      return;
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

    Try<MasterDetector*> create =
      MasterDetector::create(pid.isSome() ? string(pid.get()) : master);

    if (create.isError()) {
      error("Failed to create a master detector:" + create.error());
      return;
    }

    // Save the detector so we can delete it later.
    detector = create.get();
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

  // TODO(benh): Move this to 'protected'.
  using ProtobufProcess<MesosProcess>::send;

  void send(const Call& call)
  {
    // NOTE: We enqueue the calls to guarantee that a call is sent only after
    // a response has been received for the previous call.
    // TODO(vinod): Use HTTP pipelining instead.
    calls.push(call);

    if (calls.size() > 1) {
      return;
    }

    // If this is the first in the queue send the call.
    _send(call)
      .onAny(defer(self(), &Self::___send));
  }

protected:
  virtual void initialize()
  {
    // Start detecting masters.
    detector->detect()
      .onAny(defer(self(), &MesosProcess::detected, lambda::_1));
  }

  void detected(const Future<Option<mesos::MasterInfo>>& future)
  {
    CHECK(!future.isDiscarded());

    if (future.isFailed()) {
      error("Failed to detect a master: " + future.failure());
      return;
    }

    // Disconnect the reader upon a master detection callback.
    disconnect();

    if (future.get().isNone()) {
      master = None();

      VLOG(1) << "No master detected";

      mutex.lock()
        .then(defer(self(), &Self::_detected))
        .onAny(lambda::bind(&Mutex::unlock, mutex));
    } else {
      master = UPID(future.get().get().pid());

      VLOG(1) << "New master detected at " << master.get();

      mutex.lock()
        .then(defer(self(), &Self::__detected))
        .onAny(lambda::bind(&Mutex::unlock, mutex));
    }

    // Keep detecting masters.
    detector->detect(future.get())
      .onAny(defer(self(), &MesosProcess::detected, lambda::_1));
  }

  Future<Nothing> _detected()
  {
    return async(disconnected);
  }

  Future<Nothing> __detected()
  {
    return async(connected);
  }

  Future<Nothing> _receive()
  {
    Future<Nothing> future = async(received, events);
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

  Future<Nothing> _send(const Call& call)
  {
    if (master.isNone()) {
      drop(call, "Disconnected");
      return Nothing();
    }

    Option<Error> error = validation::scheduler::call::validate(devolve(call));

    if (error.isSome()) {
      drop(call, error.get().message);
      return Nothing();
    }

    VLOG(1) << "Sending " << call.type() << " call to " << master.get();

    // TODO(vinod): Add support for sending MESSAGE calls directly
    // to the slave, instead of relaying it through the master, as
    // the scheduler driver does.

    const string body = serialize(contentType, call);
    const hashmap<string, string> headers{{"Accept", stringify(contentType)}};

    Future<Response> response;

    if (call.type() == Call::SUBSCRIBE) {
      // Each subscription requires a new connection.
      disconnect();

      // Send a streaming request for Subscribe call.
      response = process::http::streaming::post(
          master.get(),
          "api/v1/scheduler",
          headers,
          body,
          stringify(contentType));
    } else {
      response = post(
          master.get(),
          "api/v1/scheduler",
          headers,
          body,
          stringify(contentType));
    }

    return response
      .onAny(defer(self(), &Self::__send, call, lambda::_1))
      .then([]() { return Nothing(); });
  }

  void __send(const Call& call, const Future<Response>& response)
  {
    CHECK(!response.isDiscarded());

    // This can happen during a master failover or a network blip
    // causing the socket to timeout. Eventually, the scheduler would
    // detect the disconnection via ZK(disconnect()) or lack of heartbeats.
    if (response.isFailed()) {
      LOG(ERROR) << "Request for call type " << call.type() << " failed: "
                 << response.failure();
      return;
    }

    if (response.get().status == process::http::statuses[200]) {
      // Only SUBSCRIBE call should get a "200 OK" response.
      CHECK_EQ(Call::SUBSCRIBE, call.type());
      CHECK_EQ(response.get().type, http::Response::PIPE);
      CHECK_SOME(response.get().reader);

      Pipe::Reader reader = response.get().reader.get();

      auto deserializer =
        lambda::bind(deserialize<Event>, contentType, lambda::_1);

      Owned<Reader<Event>> decoder(
          new Reader<Event>(Decoder<Event>(deserializer), reader));

      connection = Connection {reader, decoder};

      read();

      return;
    }

    if (response.get().status == process::http::statuses[202]) {
      // Only non SUBSCRIBE calls should get a "202 Accepted" response.
      CHECK_NE(Call::SUBSCRIBE, call.type());
      return;
    }

    if (response.get().status == process::http::statuses[503]) {
      // This could happen if the master hasn't realized it is the leader yet
      // or is still in the process of recovery.
      LOG(WARNING) << "Received '" << response.get().status << "' ("
                   << response.get().body << ") for " << call.type();
      return;
    }

    // We should be able to get here only for AuthN errors which is not
    // yet supported for HTTP frameworks.
    error("Received unexpected '" + response.get().status + "' (" +
          response.get().body + ") for " + stringify(call.type()));
  }

  void ___send()
  {
    CHECK_LT(0u, calls.size());
    calls.pop();

    // Execute the next event in the queue.
    if (!calls.empty()) {
      _send(calls.front())
        .onAny(defer(self(), &Self::___send));
    }
  }

  void read()
  {
    connection.get().decoder->read()
      .onAny(defer(self(),
                   &Self::_read,
                   connection.get().reader,
                   lambda::_1));
  }

  void _read(const Pipe::Reader& reader, const Future<Result<Event>>& event)
  {
    CHECK(!event.isDiscarded());

    // Ignore enqueued events from the previous Subscribe call reader.
    if (!connection.isSome() || connection.get().reader != reader) {
      VLOG(1) << "Ignoring event from old stale connection";
      return;
    }

    // This could happen if the master failed over while sending a response.
    // It's fine to drop this as the scheduler would detect the
    // disconnection via ZK(disconnect) or lack of heartbeats.
    if (event.isFailed()) {
      LOG(ERROR) << "Failed to decode the stream of events: "
                 << event.failure();
      return;
    }

    if (!event.get().isSome()) {
      // It's fine to drop this as the scheduler would detect the
      // disconnection via ZK(disconnect) or lack of heartbeats.
      LOG(ERROR) << "End-Of-File received from master."
                 << " The master closed the event stream";
      return;
    }

    if (event.get().isError()) {
      error("Failed to de-serialize event: " + event.get().error());
    } else {
      receive(event.get().get(), false);
    }

    read();
  }

  void receive(const Event& event, bool isLocallyInjected)
  {
    // Check if we're disconnected but received an event.
    if (!isLocallyInjected && master.isNone()) {
      LOG(WARNING) << "Ignoring " << stringify(event.type())
                   << " event because we're disconnected";
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

  void disconnect()
  {
    if (connection.isSome()) {
      if (!connection.get().reader.close()) {
        LOG(WARNING) << "HTTP connection was already closed";
      }
    }

    connection = None();
  }

private:
  struct Connection
  {
    Pipe::Reader reader;
    process::Owned<Reader<Event>> decoder;
  };

  Option<Connection> connection;

  ContentType contentType;

  Mutex mutex; // Used to serialize the callback invocations.

  lambda::function<void(void)> connected;
  lambda::function<void(void)> disconnected;
  lambda::function<void(const queue<Event>&)> received;

  bool local; // Whether or not we launched a local cluster.

  MasterDetector* detector;

  queue<Event> events;

  queue<Call> calls;

  Option<UPID> master;
};


Mesos::Mesos(
    const string& master,
    ContentType contentType,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
{
  process = new MesosProcess(
      master,
      contentType,
      connected,
      disconnected,
      received);

  spawn(process);
}


// Default ContentType is protobuf for HTTP requests.
Mesos::Mesos(
    const string& master,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
  : Mesos(master, ContentType::PROTOBUF, connected, disconnected, received) {}


Mesos::~Mesos()
{
  terminate(process);
  wait(process);
  delete process;
}


void Mesos::send(const Call& call)
{
  dispatch(process, &MesosProcess::send, call);
}

} // namespace scheduler {
} // namespace v1 {
} // namespace mesos {
