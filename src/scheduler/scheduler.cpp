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
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

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

using process::wait; // Necessary on some OS's to disambiguate.

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
      const lambda::function<void(void)>& _connected,
      const lambda::function<void(void)>& _disconnected,
      lambda::function<void(const queue<Event>&)> _received)
    : ProcessBase(ID::generate("scheduler")),
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
    // Check and see if we need to shutdown a local cluster.
    if (local) {
      local::shutdown();
    }

    // Wait for any callbacks to finish.
    mutex.lock().await();
  }

  // TODO(benh): Move this to 'protected'.
  using ProtobufProcess<MesosProcess>::send;

  void send(Call call)
  {
    if (master.isNone()) {
      drop(call, "Disconnected");
      return;
    }

    Option<Error> error = validation::scheduler::call::validate(devolve(call));

    if (error.isSome()) {
      drop(call, error.get().message);
      return;
    }

    // TODO(vinod): Add support for sending MESSAGE calls directly
    // to the slave, instead of relaying it through the master, as
    // the scheduler driver does.
    send(master.get(), devolve(call));
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

    receive(None(), event);
  }

  void drop(const Call& call, const string& message)
  {
    LOG(WARNING) << "Dropping " << call.type() << ": " << message;
  }

private:
  Mutex mutex; // Used to serialize the callback invocations.

  lambda::function<void(void)> connected;
  lambda::function<void(void)> disconnected;
  lambda::function<void(const queue<Event>&)> received;

  bool local; // Whether or not we launched a local cluster.

  MasterDetector* detector;

  queue<Event> events;

  Option<UPID> master;
};


Mesos::Mesos(
    const string& master,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
{
  process =
    new MesosProcess(master, connected, disconnected, received);
  spawn(process);
}


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
