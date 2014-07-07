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

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <process/async.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/mutex.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "sasl/authenticatee.hpp"

#include "common/type_utils.hpp"

#include "master/detector.hpp"

#include "local/local.hpp"

#include "master/detector.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

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
namespace scheduler {

// The process (below) is responsible for receiving messages
// (eventually events) from the master and sending messages (via
// calls) to the master.
class MesosProcess : public ProtobufProcess<MesosProcess>
{
public:
  MesosProcess(
      const string& master,
      const Option<Credential>& _credential,
      const lambda::function<void(void)>& _connected,
      const lambda::function<void(void)>& _disconnected,
      lambda::function<void(const queue<Event>&)> _received)
    : ProcessBase(ID::generate("scheduler")),
      credential(_credential),
      connected(_connected),
      disconnected(_disconnected),
      received(_received),
      local(false),
      failover(true),
      detector(NULL),
      authenticatee(NULL),
      authenticating(None()),
      authenticated(false),
      reauthenticate(false)
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

    logging::initialize("mesos", flags);

    LOG(INFO) << "Version: " << MESOS_VERSION;

    // Launch a local cluster if necessary.
    Option<UPID> pid = None();
    if (master == "local") {
      pid = local::launch(flags);
      local = true;
    }

    Try<MasterDetector*> detector_ =
      MasterDetector::create(pid.isSome() ? string(pid.get()) : master);

    if (detector_.isError()) {
      error("Failed to create a master detector:" + detector_.error());
      return;
    }

    // Save the detector so we can delete it later.
    detector = detector_.get();
  }

  virtual ~MesosProcess()
  {
    delete authenticatee;

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

    // If no user was specified in FrameworkInfo, use the current user.
    // TODO(benh): Make FrameworkInfo.user be optional and add a
    // 'user' to either TaskInfo or CommandInfo.
    if (call.framework_info().user() == "") {
      Result<string> user = os::user();
      CHECK_SOME(user);

      call.mutable_framework_info()->set_user(user.get());
    }

    // Only a REGISTER should not have set the framework ID.
    if (call.type() != Call::REGISTER && !call.framework_info().has_id()) {
      drop(call, "Call is mising FrameworkInfo.id");
      return;
    }

    if (!call.IsInitialized()) {
      drop(call, "Call is not properly initialized: " +
           call.InitializationErrorString());
      return;
    }

    switch (call.type()) {
      case Call::REGISTER: {
        RegisterFrameworkMessage message;
        message.mutable_framework()->CopyFrom(call.framework_info());
        send(master.get(), message);
        break;
      }

      case Call::REREGISTER: {
        ReregisterFrameworkMessage message;
        message.mutable_framework()->CopyFrom(call.framework_info());
        message.set_failover(failover);
        send(master.get(), message);
        break;
      }

      case Call::UNREGISTER: {
        UnregisterFrameworkMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        send(master.get(), message);
        break;
      }

      case Call::REQUEST: {
        if (!call.has_request()) {
          drop(call, "Expecting 'request' to be present");
          return;
        }
        ResourceRequestMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_requests()->CopyFrom(call.request().requests());
        send(master.get(), message);
        break;
      }

      case Call::DECLINE: {
        if (!call.has_decline()) {
          drop(call, "Expecting 'decline' to be present");
          return;
        }
        LaunchTasksMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_filters()->CopyFrom(call.decline().filters());
        message.mutable_offer_ids()->CopyFrom(call.decline().offer_ids());
        send(master.get(), message);
        break;
      }

      case Call::REVIVE: {
        ReviveOffersMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        send(master.get(), message);
        break;
      }

      case Call::LAUNCH: {
        if (!call.has_launch()) {
          drop(call, "Expecting 'launch' to be present");
          return;
        }
        // We do some local validation here, but really this should
        // all happen in the master so it's only implemented once.
        vector<TaskInfo> tasks;

        foreach (const TaskInfo& task, call.launch().task_infos()) {
          // Check that each TaskInfo has either an ExecutorInfo or a
          // CommandInfo but not both.
          if (task.has_executor() == task.has_command()) {
            drop(task,
                 "TaskInfo must have either an 'executor' or a 'command'");
            continue;
          }

          // Ensure ExecutorInfo::framework_id is valid, if present.
          if (task.has_executor() &&
              task.executor().has_framework_id() &&
              !(task.executor().framework_id() == call.framework_info().id())) {
            drop(task,
                 "ExecutorInfo has an invalid FrameworkID (Actual: " +
                 stringify(task.executor().framework_id()) + " vs Expected: " +
                 stringify(call.framework_info().id()) + ")");
            continue;
          }

          tasks.push_back(task);

          // Set ExecutorInfo::framework_id if missing since this
          // field was added to the API later and thus was made
          // optional.
          if (task.has_executor() && !task.executor().has_framework_id()) {
            tasks.back().mutable_executor()->mutable_framework_id()->CopyFrom(
                call.framework_info().id());
          }
        }

        LaunchTasksMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_filters()->CopyFrom(call.launch().filters());
        message.mutable_offer_ids()->CopyFrom(call.launch().offer_ids());

        foreach (const TaskInfo& task, tasks) {
          message.add_tasks()->CopyFrom(task);
        }

        send(master.get(), message);
        break;
      }

      case Call::KILL: {
        if (!call.has_kill()) {
          drop(call, "Expecting 'kill' to be present");
          return;
        }
        KillTaskMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_task_id()->CopyFrom(call.kill().task_id());
        send(master.get(), message);
        break;
      }

      case Call::ACKNOWLEDGE: {
        if (!call.has_acknowledge()) {
          drop(call, "Expecting 'acknowledge' to be present");
          return;
        }
        StatusUpdateAcknowledgementMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_slave_id()->CopyFrom(call.acknowledge().slave_id());
        message.mutable_task_id()->CopyFrom(call.acknowledge().task_id());
        message.set_uuid(call.acknowledge().uuid());
        send(master.get(), message);
        break;
      }

      case Call::RECONCILE: {
        if (!call.has_reconcile()) {
          drop(call, "Expecting 'reconcile' to be present");
          return;
        }
        ReconcileTasksMessage message;
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_statuses()->CopyFrom(call.reconcile().statuses());
        send(master.get(), message);
        break;
      }

      case Call::MESSAGE: {
        if (!call.has_message()) {
          drop(call, "Expecting 'message' to be present");
          return;
        }
        FrameworkToExecutorMessage message;
        message.mutable_slave_id()->CopyFrom(call.message().slave_id());
        message.mutable_framework_id()->CopyFrom(call.framework_info().id());
        message.mutable_executor_id()->CopyFrom(call.message().executor_id());
        message.set_data(call.message().data());
        send(master.get(), message);
        break;
      }

      default:
        VLOG(1) << "Unexpected call " << stringify(call.type());
        break;
    }
  }

protected:
  virtual void initialize()
  {
    install<FrameworkRegisteredMessage>(&MesosProcess::receive);
    install<FrameworkReregisteredMessage>(&MesosProcess::receive);
    install<ResourceOffersMessage>(&MesosProcess::receive);
    install<RescindResourceOfferMessage>(&MesosProcess::receive);
    install<StatusUpdateMessage>(&MesosProcess::receive);
    install<LostSlaveMessage>(&MesosProcess::receive);
    install<ExecutorToFrameworkMessage>(&MesosProcess::receive);
    install<FrameworkErrorMessage>(&MesosProcess::receive);

    // Start detecting masters.
    detector->detect()
      .onAny(defer(self(), &MesosProcess::detected, lambda::_1));
  }

  void detected(const Future<Option<MasterInfo> >& future)
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

      if (credential.isSome()) {
        // Authenticate with the master.
        authenticate();
      } else {
        mutex.lock()
          .then(defer(self(), &Self::__detected))
          .onAny(lambda::bind(&Mutex::unlock, mutex));
      }
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

  void authenticate()
  {
    authenticated = false;

    // We retry to authenticate and it's possible that we'll get
    // disconnected while that is happening.
    if (master.isNone()) {
      return;
    }

    if (authenticating.isSome()) {
      // Authentication is in progress. Try to cancel it.
      // Note that it is possible that 'authenticating' is ready
      // and the dispatch to '_authenticate' is enqueued when we
      // are here, making the 'discard' here a no-op. This is ok
      // because we set 'reauthenticate' here which enforces a retry
      // in '_authenticate'.
      Future<bool>(authenticating.get()).discard();
      reauthenticate = true;
      return;
    }

    VLOG(1) << "Authenticating with master " << master.get();

    CHECK_SOME(credential);

    CHECK(authenticatee == NULL);
    authenticatee = new sasl::Authenticatee(credential.get(), self());

    // NOTE: We do not pass 'Owned<Authenticatee>' here because doing
    // so could make 'AuthenticateeProcess' responsible for deleting
    // 'Authenticatee' causing a deadlock because the destructor of
    // 'Authenticatee' waits on 'AuthenticateeProcess'.
    // This will happen in the following scenario:
    // --> 'AuthenticateeProcess' does a 'Future.set()'.
    // --> '_authenticate()' is dispatched to this process.
    // --> This process executes '_authenticatee()'.
    // --> 'AuthenticateeProcess' removes the onAny callback
    //     from its queue which holds the last reference to
    //     'Authenticatee'.
    // --> '~Authenticatee()' is invoked by 'AuthenticateeProcess'.
    // TODO(vinod): Consider using 'Shared' to 'Owned' upgrade.
    authenticating = authenticatee->authenticate(master.get())
      .onAny(defer(self(), &Self::_authenticate));

    delay(Seconds(5),
          self(),
          &Self::authenticationTimeout,
          authenticating.get());
  }

  void _authenticate()
  {
    delete CHECK_NOTNULL(authenticatee);
    authenticatee = NULL;

    CHECK_SOME(authenticating);
    const Future<bool>& future = authenticating.get();

    if (master.isNone()) {
      VLOG(1) << "Ignoring authentication because no master is detected";
      authenticating = None();

      // Set it to false because we do not want further retries until
      // a new master is detected.
      // We obviously do not need to reauthenticate either even if
      // 'reauthenticate' is currently true because the master is
      // lost.
      reauthenticate = false;
      return;
    }

    if (reauthenticate || !future.isReady()) {
      VLOG(1)
        << "Failed to authenticate with master " << master.get() << ": "
        << (reauthenticate ? "master changed" :
           (future.isFailed() ? future.failure() : "future discarded"));

      authenticating = None();
      reauthenticate = false;

      // TODO(vinod): Add a limit on number of retries.
      dispatch(self(), &Self::authenticate); // Retry.
      return;
    }

    if (!future.get()) {
      VLOG(1) << "Master " << master.get() << " refused authentication";
      error("Authentication refused");
      return;
    }

    VLOG(1) << "Successfully authenticated with master " << master.get();

    authenticated = true;
    authenticating = None();

    mutex.lock()
      .then(defer(self(), &Self::__authenticate))
      .onAny(lambda::bind(&Mutex::unlock, mutex));
  }

  Future<Nothing> __authenticate()
  {
    return async(connected);
  }

  void authenticationTimeout(Future<bool> future)
  {
    // NOTE: Discarded future results in a retry in '_authenticate()'.
    // Also note that a 'discard' here is safe even if another
    // authenticator is in progress because this copy of the future
    // corresponds to the original authenticator that started the timer.
    if (future.discard()) { // This is a no-op if the future is already ready.
      LOG(WARNING) << "Authentication timed out";
    }
  }

  // NOTE: A None 'from' is possible when an event is injected locally.
  void receive(const Option<UPID>& from, const Event& event)
  {
    // Check if we're disconnected but received an event.
    if (from.isSome() && master.isNone()) {
      // Ignore the event unless it's a registered or reregistered.
      if (event.type() != Event::REGISTERED &&
          event.type() != Event::REREGISTERED) {
        VLOG(1) << "Ignoring " << stringify(event.type())
                << " event because we're disconnected";
        return;
      }
    } else if (from.isSome() && master != from) {
      VLOG(1)
        << "Ignoring " << stringify(event.type())
        << " event because it was sent from '" << from.get()
        << "' instead of the leading master '" << master.get() << "'";
      return;
    }

    // Note that if 'from' is None we're locally injecting this event
    // so we always want to enqueue it even if we're not connected!

    VLOG(1) << "Enqueuing event " << stringify(event.type()) << " from "
            << (from.isNone() ? "(locally injected)" : from.get());

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

  Future<Nothing> _receive()
  {
    Future<Nothing> future = async(received, events);
    events = queue<Event>();
    return future;
  }

  void receive(const UPID& from, const FrameworkRegisteredMessage& message)
  {
    // We've now registered at least once with the master so we're no
    // longer failing over. See the comment where 'failover' is
    // declared for further details.
    failover = false;

    Event event;
    event.set_type(Event::REGISTERED);

    Event::Registered* registered = event.mutable_registered();

    registered->mutable_framework_id()->CopyFrom(message.framework_id());
    registered->mutable_master_info()->CopyFrom(message.master_info());

    receive(from, event);
  }

  void receive(const UPID& from, const FrameworkReregisteredMessage& message)
  {
    // We've now registered at least once with the master so we're no
    // longer failing over. See the comment where 'failover' is
    // declared for further details.
    failover = false;

    Event event;
    event.set_type(Event::REREGISTERED);

    Event::Reregistered* reregistered = event.mutable_reregistered();

    reregistered->mutable_framework_id()->CopyFrom(message.framework_id());
    reregistered->mutable_master_info()->CopyFrom(message.master_info());

    receive(from, event);
  }

  void receive(const UPID& from, const ResourceOffersMessage& message)
  {
    Event event;
    event.set_type(Event::OFFERS);

    Event::Offers* offers = event.mutable_offers();

    offers->mutable_offers()->CopyFrom(message.offers());

    receive(from, event);
  }

  void receive(const UPID& from, const RescindResourceOfferMessage& message)
  {
    Event event;
    event.set_type(Event::RESCIND);

    Event::Rescind* rescind = event.mutable_rescind();

    rescind->mutable_offer_id()->CopyFrom(message.offer_id());

    receive(from, event);
  }

  void receive(const UPID& from, const StatusUpdateMessage& message)
  {
    Event event;
    event.set_type(Event::UPDATE);

    Event::Update* update = event.mutable_update();

    update->mutable_status()->CopyFrom(message.update().status());

    if (message.update().has_slave_id()) {
      update->mutable_status()->mutable_slave_id()->CopyFrom(
          message.update().slave_id());
    }

    if (message.update().has_executor_id()) {
      update->mutable_status()->mutable_executor_id()->CopyFrom(
          message.update().executor_id());
    }

    update->mutable_status()->set_timestamp(message.update().timestamp());

    update->set_uuid(message.update().uuid());

    receive(from, event);
  }

  void receive(const UPID& from, const LostSlaveMessage& message)
  {
    Event event;
    event.set_type(Event::FAILURE);

    Event::Failure* failure = event.mutable_failure();

    failure->mutable_slave_id()->CopyFrom(message.slave_id());

    receive(from, event);
  }

  void receive(const UPID& from, const ExecutorToFrameworkMessage& _message)
  {
    Event event;
    event.set_type(Event::MESSAGE);

    Event::Message* message = event.mutable_message();

    message->mutable_slave_id()->CopyFrom(_message.slave_id());
    message->mutable_executor_id()->CopyFrom(_message.executor_id());
    message->set_data(_message.data());

    receive(from, event);
  }

  void receive(const UPID& from, const FrameworkErrorMessage& message)
  {
    Event event;
    event.set_type(Event::ERROR);

    Event::Error* error = event.mutable_error();

    error->set_message(message.message());

    receive(from, event);
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

  // Helper for "dropping" a task that was launched.
  void drop(const TaskInfo& task, const string& message)
  {
    Event event;

    event.set_type(Event::UPDATE);

    Event::Update* update = event.mutable_update();

    TaskStatus* status = update->mutable_status();
    status->mutable_task_id()->CopyFrom(task.task_id());
    status->set_state(TASK_LOST);
    status->set_message(message);
    status->set_timestamp(Clock::now().secs());

    update->set_uuid(UUID::random().toBytes());

    receive(None(), event);
  }

  void drop(const Call& call, const string& message)
  {
    VLOG(1) << "Dropping " << stringify(call.type()) << ": " << message;

    switch (call.type()) {
      case Call::LAUNCH: {
        // We drop the tasks preemptively (enqueing update events that
        // put the task in TASK_LOST). This is a hack for now, to keep
        // the tasks from being forever in PENDING state, when
        // actually the master never received the launch.
        // Unfortuantely this is insufficient since it doesn't capture
        // the case when the scheduler process sends it but the master
        // never receives it (i.e., during a master failover). In the
        // future, this should be solved by higher-level abstractions
        // and this hack should be considered for removal.
        foreach (const TaskInfo& task, call.launch().task_infos()) {
          drop(task, message);
        }
        break;
      }

      default:
        break;
    }
  }

private:
  const Option<Credential> credential;

  Mutex mutex; // Used to serialize the callback invocations.

  lambda::function<void(void)> connected;
  lambda::function<void(void)> disconnected;
  lambda::function<void(const queue<Event>&)> received;

  bool local; // Whether or not we launched a local cluster.

  // Whether or not this is the first time we've sent a
  // REREGISTER. This is to maintain compatibility with what the
  // master expects from SchedulerProcess. After the first REGISTER or
  // REREGISTER event we force this to be false.
  bool failover;

  MasterDetector* detector;

  queue<Event> events;

  Option<UPID> master;

  sasl::Authenticatee* authenticatee;

  // Indicates if an authentication attempt is in progress.
  Option<Future<bool> > authenticating;

  // Indicates if the authentication is successful.
  bool authenticated;

  // Indicates if a new authentication attempt should be enforced.
  bool reauthenticate;
};


Mesos::Mesos(
    const string& master,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
{
  process =
    new MesosProcess(master, None(), connected, disconnected, received);
  spawn(process);
}


Mesos::Mesos(
    const string& master,
    const Credential& credential,
    const lambda::function<void(void)>& connected,
    const lambda::function<void(void)>& disconnected,
    const lambda::function<void(const queue<Event>&)>& received)
{
  process =
    new MesosProcess(master, credential, connected, disconnected, received);
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
} // namespace mesos {
