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
#include <map>
#include <string>
#include <sstream>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authentication/authenticatee.hpp>

#include <mesos/module/authenticatee.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/abort.hpp>
#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/hashmap.hpp>
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "authentication/cram_md5/authenticatee.hpp"

#include "common/protobuf_utils.hpp"

#include "local/flags.hpp"
#include "local/local.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/detector.hpp"

#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "sched/constants.hpp"
#include "sched/flags.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;
using namespace mesos::scheduler;

using namespace process;

using std::map;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

using utils::copy;

namespace mesos {
namespace internal {

// The scheduler process (below) is responsible for interacting with
// the master and responding to Mesos API calls from scheduler
// drivers. In order to allow a message to be sent back to the master
// we allow friend functions to invoke 'send', 'post', etc. Therefore,
// we must make sure that any necessary synchronization is performed.

class SchedulerProcess : public ProtobufProcess<SchedulerProcess>
{
public:
  SchedulerProcess(MesosSchedulerDriver* _driver,
                   Scheduler* _scheduler,
                   const FrameworkInfo& _framework,
                   const Option<Credential>& _credential,
                   bool _implicitAcknowledgements,
                   const string& schedulerId,
                   MasterDetector* _detector,
                   const internal::scheduler::Flags& _flags,
                   pthread_mutex_t* _mutex,
                   pthread_cond_t* _cond)
      // We use a UUID here to ensure that the master can reliably
      // distinguish between scheduler runs. Otherwise the master may
      // receive a delayed ExitedEvent enqueued behind a
      // re-registration, and deactivate the framework incorrectly.
      // TODO(bmahler): Investigate better ways to solve this problem.
      // Check if bidirectional links in Erlang provides better
      // semantics:
      // http://www.erlang.org/doc/reference_manual/processes.html#id84804.
      // Consider using unique PIDs throughout libprocess and relying
      // on name registration to identify the process without the PID.
    : ProcessBase(schedulerId),
      metrics(*this),
      driver(_driver),
      scheduler(_scheduler),
      framework(_framework),
      mutex(_mutex),
      cond(_cond),
      failover(_framework.has_id() && !framework.id().value().empty()),
      connected(false),
      running(true),
      detector(_detector),
      flags(_flags),
      implicitAcknowledgements(_implicitAcknowledgements),
      credential(_credential),
      authenticatee(NULL),
      authenticating(None()),
      authenticated(false),
      reauthenticate(false)
  {
    LOG(INFO) << "Version: " << MESOS_VERSION;
  }

  virtual ~SchedulerProcess()
  {
    delete authenticatee;
  }

protected:
  virtual void initialize()
  {
    // TODO(benh): Get access to flags so that we can decide whether
    // or not to make ZooKeeper verbose.
    install<FrameworkRegisteredMessage>(
        &SchedulerProcess::registered,
        &FrameworkRegisteredMessage::framework_id,
        &FrameworkRegisteredMessage::master_info);

    install<FrameworkReregisteredMessage>(
        &SchedulerProcess::reregistered,
        &FrameworkReregisteredMessage::framework_id,
        &FrameworkReregisteredMessage::master_info);

    install<ResourceOffersMessage>(
        &SchedulerProcess::resourceOffers,
        &ResourceOffersMessage::offers,
        &ResourceOffersMessage::pids);

    install<RescindResourceOfferMessage>(
        &SchedulerProcess::rescindOffer,
        &RescindResourceOfferMessage::offer_id);

    install<StatusUpdateMessage>(
        &SchedulerProcess::statusUpdate,
        &StatusUpdateMessage::update,
        &StatusUpdateMessage::pid);

    install<LostSlaveMessage>(
        &SchedulerProcess::lostSlave,
        &LostSlaveMessage::slave_id);

    install<ExecutorToFrameworkMessage>(
        &SchedulerProcess::frameworkMessage,
        &ExecutorToFrameworkMessage::slave_id,
        &ExecutorToFrameworkMessage::framework_id,
        &ExecutorToFrameworkMessage::executor_id,
        &ExecutorToFrameworkMessage::data);

    install<FrameworkErrorMessage>(
        &SchedulerProcess::error,
        &FrameworkErrorMessage::message);

    // Start detecting masters.
    detector->detect()
      .onAny(defer(self(), &SchedulerProcess::detected, lambda::_1));
  }

  void detected(const Future<Option<MasterInfo> >& _master)
  {
    if (!running) {
      VLOG(1) << "Ignoring the master change because the driver is not"
              << " running!";
      return;
    }

    CHECK(!_master.isDiscarded());

    if (_master.isFailed()) {
      EXIT(1) << "Failed to detect a master: " << _master.failure();
    }

    if (_master.get().isSome()) {
      master = UPID(_master.get().get().pid());
    } else {
      master = None();
    }

    if (connected) {
      // There are three cases here:
      //   1. The master failed.
      //   2. The master failed over to a new master.
      //   3. The master failed over to the same master.
      // In any case, we will reconnect (possibly immediately), so we
      // must notify schedulers of the disconnection.
      Stopwatch stopwatch;
      if (FLAGS_v >= 1) {
        stopwatch.start();
      }

      scheduler->disconnected(driver);

      VLOG(1) << "Scheduler::disconnected took " << stopwatch.elapsed();
    }

    connected = false;

    if (master.isSome()) {
      LOG(INFO) << "New master detected at " << master.get();
      link(master.get());

      if (credential.isSome()) {
        // Authenticate with the master.
        // TODO(vinod): Do a backoff for authentication similar to what
        // we do for registration.
        authenticate();
      } else {
        // Proceed with registration without authentication.
        LOG(INFO) << "No credentials provided."
                  << " Attempting to register without authentication";

        // TODO(vinod): Similar to the slave add a random delay to the
        // first registration attempt too. This needs fixing tests
        // that expect scheduler to register even with clock paused
        // (e.g., rate limiting tests).
        doReliableRegistration(flags.registration_backoff_factor);
      }
    } else {
      // In this case, we don't actually invoke Scheduler::error
      // since we might get reconnected to a master imminently.
      LOG(INFO) << "No master detected";
    }

    // Keep detecting masters.
    detector->detect(_master.get())
      .onAny(defer(self(), &SchedulerProcess::detected, lambda::_1));
  }


  void authenticate()
  {
    if (!running) {
      VLOG(1) << "Ignoring authenticate because the driver is not running!";
      return;
    }

    authenticated = false;

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
      copy(authenticating.get()).discard();
      reauthenticate = true;
      return;
    }

    LOG(INFO) << "Authenticating with master " << master.get();

    CHECK_SOME(credential);

    CHECK(authenticatee == NULL);

    if (flags.authenticatee == scheduler::DEFAULT_AUTHENTICATEE) {
      LOG(INFO) << "Using default CRAM-MD5 authenticatee";
      authenticatee = new cram_md5::CRAMMD5Authenticatee();
    } else {
      Try<Authenticatee*> module =
        modules::ModuleManager::create<Authenticatee>(flags.authenticatee);
      if (module.isError()) {
        EXIT(1) << "Could not create authenticatee module '"
                << flags.authenticatee << "': " << module.error();
      }
      LOG(INFO) << "Using '" << flags.authenticatee << "' authenticatee";
      authenticatee = module.get();
    }

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
    authenticating =
      authenticatee->authenticate(master.get(), self(), credential.get())
        .onAny(defer(self(), &Self::_authenticate));

    delay(Seconds(5),
          self(),
          &Self::authenticationTimeout,
          authenticating.get());
  }

  void _authenticate()
  {
    if (!running) {
      VLOG(1) << "Ignoring _authenticate because the driver is not running!";
      return;
    }

    delete CHECK_NOTNULL(authenticatee);
    authenticatee = NULL;

    CHECK_SOME(authenticating);
    const Future<bool>& future = authenticating.get();

    if (master.isNone()) {
      LOG(INFO) << "Ignoring _authenticate because the master is lost";
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
      LOG(INFO)
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
      LOG(ERROR) << "Master " << master.get() << " refused authentication";
      error("Master refused authentication");
      return;
    }

    LOG(INFO) << "Successfully authenticated with master " << master.get();

    authenticated = true;
    authenticating = None();

    doReliableRegistration(flags.registration_backoff_factor);
  }

  void authenticationTimeout(Future<bool> future)
  {
    if (!running) {
      VLOG(1) << "Ignoring authentication timeout because "
              << "the driver is not running!";
      return;
    }

    // NOTE: Discarded future results in a retry in '_authenticate()'.
    // Also note that a 'discard' here is safe even if another
    // authenticator is in progress because this copy of the future
    // corresponds to the original authenticator that started the timer.
    if (future.discard()) { // This is a no-op if the future is already ready.
      LOG(WARNING) << "Authentication timed out";
    }
  }

  void registered(
      const UPID& from,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo)
  {
    if (!running) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is not running!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is already connected!";
      return;
    }

    if (master != from) {
      LOG(WARNING)
        << "Ignoring framework registered message because it was sent "
        << "from '" << from << "' instead of the leading master '"
        << (master.isSome() ? master.get() : UPID()) << "'";
      return;
    }

    LOG(INFO) << "Framework registered with " << frameworkId;

    framework.mutable_id()->MergeFrom(frameworkId);

    connected = true;
    failover = false;

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->registered(driver, frameworkId, masterInfo);

    VLOG(1) << "Scheduler::registered took " << stopwatch.elapsed();
  }

  void reregistered(
      const UPID& from,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo)
  {
    if (!running) {
      VLOG(1) << "Ignoring framework re-registered message because "
              << "the driver is not running!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework re-registered message because "
              << "the driver is already connected!";
      return;
    }

    if (master != from) {
      LOG(WARNING)
        << "Ignoring framework re-registered message because it was sent "
        << "from '" << from << "' instead of the leading master '"
        << (master.isSome() ? master.get() : UPID()) << "'";
      return;
    }

    LOG(INFO) << "Framework re-registered with " << frameworkId;

    CHECK(framework.id() == frameworkId);

    connected = true;
    failover = false;

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->reregistered(driver, masterInfo);

    VLOG(1) << "Scheduler::reregistered took " << stopwatch.elapsed();
  }

  void doReliableRegistration(Duration maxBackoff)
  {
    if (!running) {
      return;
    }

    if (connected || master.isNone()) {
      return;
    }

    if (credential.isSome() && !authenticated) {
      return;
    }

    VLOG(1) << "Sending registration request to " << master.get();

    if (!framework.has_id() || framework.id() == "") {
      // Touched for the very first time.
      RegisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      send(master.get(), message);
    } else {
      // Not the first time, or failing over.
      ReregisterFrameworkMessage message;
      message.mutable_framework()->MergeFrom(framework);
      message.set_failover(failover);
      send(master.get(), message);
    }

    // Bound the maximum backoff by 'REGISTRATION_RETRY_INTERVAL_MAX'.
    maxBackoff =
      std::min(maxBackoff, scheduler::REGISTRATION_RETRY_INTERVAL_MAX);

    // If failover timeout is present, bound the maximum backoff
    // by 1/10th of the failover timeout.
    if (framework.has_failover_timeout()) {
      Try<Duration> duration = Duration::create(framework.failover_timeout());
      if (duration.isSome()) {
        maxBackoff = std::min(maxBackoff, duration.get() / 10);
      }
    }

    // Determine the delay for next attempt by picking a random
    // duration between 0 and 'maxBackoff'.
    // TODO(vinod): Use random numbers from <random> header.
    Duration delay = maxBackoff * ((double) ::random() / RAND_MAX);

    VLOG(1) << "Will retry registration in " << delay << " if necessary";

    // Backoff.
    process::delay(
        delay, self(), &Self::doReliableRegistration, maxBackoff * 2);
  }

  void resourceOffers(
      const UPID& from,
      const vector<Offer>& offers,
      const vector<string>& pids)
  {
    if (!running) {
      VLOG(1) << "Ignoring resource offers message because "
              << "the driver is not running!";
      return;
    }

    if (!connected) {
      VLOG(1) << "Ignoring resource offers message because the driver is "
              << "disconnected!";
      return;
    }

    CHECK_SOME(master);

    if (from != master.get()) {
      VLOG(1) << "Ignoring resource offers message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master.get() << "'";
      return;
    }

    VLOG(2) << "Received " << offers.size() << " offers";

    CHECK(offers.size() == pids.size());

    // Save the pid associated with each slave (one per offer) so
    // later we can send framework messages directly.
    for (size_t i = 0; i < offers.size(); i++) {
      UPID pid(pids[i]);
      // Check if parse failed (e.g., due to DNS).
      if (pid != UPID()) {
        VLOG(3) << "Saving PID '" << pids[i] << "'";
        savedOffers[offers[i].id()][offers[i].slave_id()] = pid;
      } else {
        VLOG(1) << "Failed to parse PID '" << pids[i] << "'";
      }
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->resourceOffers(driver, offers);

    VLOG(1) << "Scheduler::resourceOffers took " << stopwatch.elapsed();
  }

  void rescindOffer(const UPID& from, const OfferID& offerId)
  {
    if (!running) {
      VLOG(1) << "Ignoring rescind offer message because "
              << "the driver is not running!";
      return;
    }

    if (!connected) {
      VLOG(1) << "Ignoring rescind offer message because the driver is "
              << "disconnected!";
      return;
    }

    CHECK_SOME(master);

    if (from != master.get()) {
      VLOG(1) << "Ignoring rescind offer message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master.get() << "'";
      return;
    }

    VLOG(1) << "Rescinded offer " << offerId;

    savedOffers.erase(offerId);

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->offerRescinded(driver, offerId);

    VLOG(1) << "Scheduler::offerRescinded took " << stopwatch.elapsed();
  }

  void statusUpdate(
      const UPID& from,
      const StatusUpdate& update,
      const UPID& pid)
  {
    if (!running) {
      VLOG(1) << "Ignoring task status update message because "
              << "the driver is not running!";
      return;
    }

    // Allow status updates created from the driver itself.
    if (from != UPID()) {
      if (!connected) {
        VLOG(1) << "Ignoring status update message because the driver is "
                << "disconnected!";
        return;
      }

      CHECK_SOME(master);

      if (from != master.get()) {
        VLOG(1) << "Ignoring status update message because it was sent "
                << "from '" << from << "' instead of the leading master '"
                << master.get() << "'";
        return;
      }
    }

    VLOG(2) << "Received status update " << update << " from " << pid;

    CHECK(framework.id() == update.framework_id());

    // TODO(benh): Note that this maybe a duplicate status update!
    // Once we get support to try and have a more consistent view
    // of what's running in the cluster, we'll just let this one
    // slide. The alternative is possibly dealing with a scheduler
    // failover and not correctly giving the scheduler it's status
    // update, which seems worse than giving a status update
    // multiple times (of course, if a scheduler re-uses a TaskID,
    // that could be bad.

    TaskStatus status = update.status();

    // If the update does not have a 'uuid', it does not need
    // acknowledging. However, prior to 0.23.0, the update uuid
    // was required and always set. In 0.24.0, we can rely on the
    // update uuid check here, until then we must still check for
    // this being sent from the driver (from == UPID()) or from
    // the master (pid == UPID()).
    //
    // TODO(bmahler): For the HTTP API, we will have to update the
    // master and slave to ensure the 'uuid' in TaskStatus is set
    // correctly.
    if (!update.has_uuid()) {
      status.clear_uuid();
    } else if (from == UPID() || pid == UPID()) {
      status.clear_uuid();
    } else {
      status.set_uuid(update.uuid());
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->statusUpdate(driver, status);

    VLOG(1) << "Scheduler::statusUpdate took " << stopwatch.elapsed();

    if (implicitAcknowledgements) {
      // Note that we need to look at the volatile 'running' here
      // so that we don't acknowledge the update if the driver was
      // aborted during the processing of the update.
      if (!running) {
        VLOG(1) << "Not sending status update acknowledgment message because "
                << "the driver is not running!";
        return;
      }

      // See above for when we don't need to acknowledge.
      if (update.has_uuid() && from != UPID() && pid != UPID()) {
        // We drop updates while we're disconnected.
        CHECK(connected);
        CHECK_SOME(master);

        VLOG(2) << "Sending ACK for status update " << update
            << " to " << master.get();

        StatusUpdateAcknowledgementMessage message;
        message.mutable_framework_id()->MergeFrom(framework.id());
        message.mutable_slave_id()->MergeFrom(update.slave_id());
        message.mutable_task_id()->MergeFrom(update.status().task_id());
        message.set_uuid(update.uuid());
        send(master.get(), message);
      }
    }
  }

  void lostSlave(const UPID& from, const SlaveID& slaveId)
  {
    if (!running) {
      VLOG(1) << "Ignoring lost slave message because the driver is not"
              << " running!";
      return;
    }

    if (!connected) {
      VLOG(1) << "Ignoring lost slave message because the driver is "
              << "disconnected!";
      return;
    }

    CHECK_SOME(master);

    if (from != master.get()) {
      VLOG(1) << "Ignoring lost slave message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master.get() << "'";
      return;
    }

    VLOG(1) << "Lost slave " << slaveId;

    savedSlavePids.erase(slaveId);

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->slaveLost(driver, slaveId);

    VLOG(1) << "Scheduler::slaveLost took " << stopwatch.elapsed();
  }

  void frameworkMessage(const SlaveID& slaveId,
                        const FrameworkID& frameworkId,
                        const ExecutorID& executorId,
                        const string& data)
  {
    if (!running) {
      VLOG(1)
        << "Ignoring framework message because the driver is not running!";
      return;
    }

    VLOG(2) << "Received framework message";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->frameworkMessage(driver, executorId, slaveId, data);

    VLOG(1) << "Scheduler::frameworkMessage took " << stopwatch.elapsed();
  }

  void error(const string& message)
  {
    if (!running) {
      VLOG(1) << "Ignoring error message because the driver is not running!";
      return;
    }

    LOG(INFO) << "Got error '" << message << "'";

    driver->abort();

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->error(driver, message);

    VLOG(1) << "Scheduler::error took " << stopwatch.elapsed();
  }

  void stop(bool failover)
  {
    LOG(INFO) << "Stopping framework '" << framework.id() << "'";

    // Whether or not we send an unregister message, we want to
    // terminate this process.
    terminate(self());

    if (connected && !failover) {
      UnregisterFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework.id());
      CHECK_SOME(master);
      send(master.get(), message);
    }

    synchronized (mutex) {
      pthread_cond_signal(cond);
    }
  }

  // NOTE: This function informs the master to stop attempting to send
  // messages to this scheduler. The abort flag stops any already
  // enqueued messages or messages in flight from being handled. We
  // don't want to terminate the process because one might do a
  // MesosSchedulerDriver::stop later, which dispatches to
  // SchedulerProcess::stop.
  void abort()
  {
    LOG(INFO) << "Aborting framework '" << framework.id() << "'";

    CHECK(!running);

    if (!connected) {
      VLOG(1) << "Not sending a deactivate message as master is disconnected";
    } else {
      DeactivateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework.id());
      CHECK_SOME(master);
      send(master.get(), message);
    }

    synchronized (mutex) {
      pthread_cond_signal(cond);
    }
  }

  void killTask(const TaskID& taskId)
  {
    if (!connected) {
      VLOG(1) << "Ignoring kill task message as master is disconnected";
      return;
    }

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    message.mutable_task_id()->MergeFrom(taskId);
    CHECK_SOME(master);
    send(master.get(), message);
  }

  void requestResources(const vector<Request>& requests)
  {
    if (!connected) {
      VLOG(1) << "Ignoring request resources message as master is disconnected";
      return;
    }

    ResourceRequestMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    foreach (const Request& request, requests) {
      message.add_requests()->MergeFrom(request);
    }
    CHECK_SOME(master);
    send(master.get(), message);
  }

  void launchTasks(const vector<OfferID>& offerIds,
                   const vector<TaskInfo>& tasks,
                   const Filters& filters)
  {
    if (!connected) {
      VLOG(1) << "Ignoring launch tasks message as master is disconnected";
      // NOTE: Reply to the framework with TASK_LOST messages for each
      // task. This is a hack for now, to not let the scheduler
      // believe the tasks are launched, when actually the master
      // never received the launchTasks message. Also, realize that
      // this hack doesn't capture the case when the scheduler process
      // sends it but the master never receives it (message lost,
      // master failover etc). The correct way for schedulers to deal
      // with this situation is to use 'reconcileTasks()'.
      foreach (const TaskInfo& task, tasks) {
        StatusUpdate update = protobuf::createStatusUpdate(
            framework.id(),
            None(),
            task.task_id(),
            TASK_LOST,
            TaskStatus::SOURCE_MASTER,
            None(),
            "Master disconnected",
            TaskStatus::REASON_MASTER_DISCONNECTED);

        statusUpdate(UPID(), update, UPID());
      }
      return;
    }

    // Set TaskInfo.executor.framework_id, if it's missing.
    // TODO(ijimenez): Remove this validation in 0.24.0.
    vector<TaskInfo> result;
    foreach (TaskInfo task, tasks) {
      if (task.has_executor() && !task.executor().has_framework_id()) {
        task.mutable_executor()->mutable_framework_id()->CopyFrom(
            framework.id());
      }
      result.push_back(task);
    }

    LaunchTasksMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    message.mutable_filters()->MergeFrom(filters);

    foreach (const OfferID& offerId, offerIds) {
      message.add_offer_ids()->MergeFrom(offerId);

      foreach (const TaskInfo& task, result) {
        // Keep only the slave PIDs where we run tasks so we can send
        // framework messages directly.
        if (savedOffers.contains(offerId)) {
          if (savedOffers[offerId].count(task.slave_id()) > 0) {
            savedSlavePids[task.slave_id()] =
              savedOffers[offerId][task.slave_id()];
          } else {
            LOG(WARNING) << "Attempting to launch task " << task.task_id()
                         << " with the wrong slave id " << task.slave_id();
          }
        } else {
          LOG(WARNING) << "Attempting to launch task " << task.task_id()
                       << " with an unknown offer " << offerId;
        }
      }
      // Remove the offer since we saved all the PIDs we might use.
      savedOffers.erase(offerId);
    }

    foreach (const TaskInfo& task, result) {
      message.add_tasks()->MergeFrom(task);
    }

    CHECK_SOME(master);
    send(master.get(), message);
  }

  void acceptOffers(
      const vector<OfferID>& offerIds,
      const vector<Offer::Operation>& operations,
      const Filters& filters)
  {
    // TODO(jieyu): Move all driver side verification to master since
    // we are moving towards supporting pure launguage scheduler.

    if (!connected) {
      VLOG(1) << "Ignoring accept offers message as master is disconnected";

      // NOTE: Reply to the framework with TASK_LOST messages for each
      // task launch. See details from notes in launchTasks.
      foreach (const Offer::Operation& operation, operations) {
        if (operation.type() != Offer::Operation::LAUNCH) {
          continue;
        }

        foreach (const TaskInfo& task, operation.launch().task_infos()) {
          StatusUpdate update = protobuf::createStatusUpdate(
              framework.id(),
              None(),
              task.task_id(),
              TASK_LOST,
              TaskStatus::SOURCE_MASTER,
              None(),
              "Master disconnected",
              TaskStatus::REASON_MASTER_DISCONNECTED);

          statusUpdate(UPID(), update, UPID());
        }
      }
      return;
    }

    Call message;
    CHECK(framework.has_id());
    message.mutable_framework_id()->CopyFrom(framework.id());
    message.set_type(Call::ACCEPT);

    Call::Accept* accept = message.mutable_accept();

    // Setting accept.operations.
    foreach (const Offer::Operation& _operation, operations) {
      Offer::Operation* operation = accept->add_operations();
      operation->CopyFrom(_operation);

      if (operation->type() != Offer::Operation::LAUNCH) {
        continue;
      }

      // Set TaskInfo.executor.framework_id, if it's missing.
      // TODO(ijimenez): Remove this validation in 0.24.0.
      foreach (TaskInfo& task,
               *operation->mutable_launch()->mutable_task_infos()) {
        if (task.has_executor() && !task.executor().has_framework_id()) {
          task.mutable_executor()->mutable_framework_id()->CopyFrom(
              framework.id());
        }
      }
    }

    // Setting accept.offer_ids.
    foreach (const OfferID& offerId, offerIds) {
      accept->add_offer_ids()->CopyFrom(offerId);

      if (!savedOffers.contains(offerId)) {
        // TODO(jieyu): A duplicated offer ID could also cause this
        // warning being printed. Consider refine this message here
        // and in launchTasks as well.
        LOG(WARNING) << "Attempting to accept an unknown offer " << offerId;
      } else {
        // Keep only the slave PIDs where we run tasks so we can send
        // framework messages directly.
        foreach (const Offer::Operation& operation, operations) {
          if (operation.type() != Offer::Operation::LAUNCH) {
            continue;
          }

          foreach (const TaskInfo& task, operation.launch().task_infos()) {
            const SlaveID& slaveId = task.slave_id();

            if (savedOffers[offerId].contains(slaveId)) {
              savedSlavePids[slaveId] = savedOffers[offerId][slaveId];
            } else {
              LOG(WARNING) << "Attempting to launch task " << task.task_id()
                           << " with the wrong slave id " << slaveId;
            }
          }
        }
      }

      // Remove the offer since we saved all the PIDs we might use.
      savedOffers.erase(offerId);
    }

    // Setting accept.filters.
    accept->mutable_filters()->CopyFrom(filters);

    CHECK_SOME(master);
    send(master.get(), message);
  }

  void reviveOffers()
  {
    if (!connected) {
      VLOG(1) << "Ignoring revive offers message as master is disconnected";
      return;
    }

    ReviveOffersMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());
    CHECK_SOME(master);
    send(master.get(), message);
  }

  void acknowledgeStatusUpdate(
      const TaskStatus& status)
  {
    // The driver should abort before allowing an acknowledgement
    // call when implicit acknowledgements are enabled. We further
    // enforce that the driver is denying the call through this CHECK.
    CHECK(!implicitAcknowledgements);

    if (!connected) {
      VLOG(1) << "Ignoring explicit status update acknowledgement"
                 " because the driver is disconnected";
      return;
    }

    CHECK_SOME(master);

    // NOTE: By ignoring the volatile 'running' here, we ensure that
    // all acknowledgements requested before the driver was stopped
    // or aborted are processed. Any acknowledgement that is requested
    // after the driver stops or aborts (running == false) will be
    // dropped in the driver before reaching here.

    // Only statuses with a 'uuid' and a 'slave_id' need to have
    // acknowledgements sent to the master. Note that the driver
    // ensures that master-generated and driver-generated updates
    // will not have a 'uuid' set.
    if (status.has_uuid() && status.has_slave_id()) {
      VLOG(2) << "Sending ACK for status update " << status.uuid()
              << " of task " << status.task_id()
              << " on slave " << status.slave_id()
              << " to " << master.get();

      StatusUpdateAcknowledgementMessage message;
      message.mutable_framework_id()->CopyFrom(framework.id());
      message.mutable_slave_id()->CopyFrom(status.slave_id());
      message.mutable_task_id()->CopyFrom(status.task_id());
      message.set_uuid(status.uuid());
      send(master.get(), message);
    } else {
      VLOG(2) << "Received ACK for status update"
              << (status.has_uuid() ? " " + status.uuid() : "")
              << " of task " << status.task_id()
              << (status.has_slave_id()
                  ? " on slave " + stringify(status.slave_id()) : "");
    }
  }

  void sendFrameworkMessage(const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            const string& data)
  {
    if (!connected) {
     VLOG(1) << "Ignoring send framework message as master is disconnected";
     return;
    }

    VLOG(2) << "Asked to send framework message to slave "
            << slaveId;

    // TODO(benh): After a scheduler has re-registered it won't have
    // any saved slave PIDs, maybe it makes sense to try and save each
    // PID that this scheduler tries to send a message to? Or we can
    // just wait for them to recollect as new offers come in and get
    // accepted.

    if (savedSlavePids.count(slaveId) > 0) {
      UPID slave = savedSlavePids[slaveId];
      CHECK(slave != UPID());

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(framework.id());
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave, message);
    } else {
      VLOG(1) << "Cannot send directly to slave " << slaveId
              << "; sending through master";

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(framework.id());
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      CHECK_SOME(master);
      send(master.get(), message);
    }
  }

  void reconcileTasks(const vector<TaskStatus>& statuses)
  {
    if (!connected) {
     VLOG(1) << "Ignoring task reconciliation as master is disconnected";
     return;
    }

    ReconcileTasksMessage message;
    message.mutable_framework_id()->MergeFrom(framework.id());

    foreach (const TaskStatus& status, statuses) {
      message.add_statuses()->MergeFrom(status);
    }

    CHECK_SOME(master);
    send(master.get(), message);
  }

private:
  friend class mesos::MesosSchedulerDriver;

  struct Metrics
  {
    Metrics(const SchedulerProcess& schedulerProcess)
      : event_queue_messages(
          "scheduler/event_queue_messages",
          defer(schedulerProcess, &SchedulerProcess::_event_queue_messages)),
        event_queue_dispatches(
          "scheduler/event_queue_dispatches",
          defer(schedulerProcess,
                &SchedulerProcess::_event_queue_dispatches))
    {
      // TODO(dhamon): When we start checking the return value of 'add' we may
      // get failures in situations where multiple SchedulerProcesses are active
      // (ie, the fault tolerance tests). At that point we'll need MESOS-1285 to
      // be fixed and to use self().id in the metric name.
      process::metrics::add(event_queue_messages);
      process::metrics::add(event_queue_dispatches);
    }

    ~Metrics()
    {
      process::metrics::remove(event_queue_messages);
      process::metrics::remove(event_queue_dispatches);
    }

    // Process metrics.
    process::metrics::Gauge event_queue_messages;
    process::metrics::Gauge event_queue_dispatches;
  } metrics;

  double _event_queue_messages()
  {
    return static_cast<double>(eventCount<MessageEvent>());
  }

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<DispatchEvent>());
  }

  MesosSchedulerDriver* driver;
  Scheduler* scheduler;
  FrameworkInfo framework;
  pthread_mutex_t* mutex;
  pthread_cond_t* cond;
  bool failover;
  Option<UPID> master;

  bool connected; // Flag to indicate if framework is registered.

  // TODO(vinod): Instead of 'bool' use 'Status'.
  // We set 'running' to false in SchedulerDriver::stop() and
  // SchedulerDriver::abort() to prevent any further messages from
  // being processed in the SchedulerProcess. However, if abort()
  // or stop() is called from a thread other than SchedulerProcess,
  // there may be one additional callback delivered to the scheduler.
  // This could happen if the SchedulerProcess is in the middle of
  // processing an event.
  // TODO(vinod): Instead of 'volatile' use std::atomic() to guarantee
  // atomicity.
  volatile bool running; // Flag to indicate if the driver is running.

  MasterDetector* detector;

  const internal::scheduler::Flags flags;

  hashmap<OfferID, hashmap<SlaveID, UPID> > savedOffers;
  hashmap<SlaveID, UPID> savedSlavePids;

  // The driver optionally provides implicit acknowledgements
  // for frameworks. If disabled, the framework must send its
  // own acknowledgements through the driver, when the 'uuid'
  // of the TaskStatus is set (which also implies the 'slave_id'
  // is set).
  bool implicitAcknowledgements;

  const Option<Credential> credential;

  Authenticatee* authenticatee;

  // Indicates if an authentication attempt is in progress.
  Option<Future<bool> > authenticating;

  // Indicates if the authentication is successful.
  bool authenticated;

  // Indicates if a new authentication attempt should be enforced.
  bool reauthenticate;
};

} // namespace internal {
} // namespace mesos {


void MesosSchedulerDriver::initialize() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Load any flags from the environment (we use local::Flags in the
  // event we run in 'local' mode, since it inherits logging::Flags).
  // In the future, just as the TODO in local/main.cpp discusses,
  // we'll probably want a way to load master::Flags and slave::Flags
  // as well.
  local::Flags flags;
  Try<Nothing> load = flags.load("MESOS_");

  if (load.isError()) {
    status = DRIVER_ABORTED;
    scheduler->error(this, load.error());
    return;
  }

  // Initialize libprocess.
  process::initialize(schedulerId);

  if (process::address().ip.isLoopback()) {
    LOG(WARNING) << "\n**************************************************\n"
                 << "Scheduler driver bound to loopback interface!"
                 << " Cannot communicate with remote master(s)."
                 << " You might want to set 'LIBPROCESS_IP' environment"
                 << " variable to use a routable IP address.\n"
                 << "**************************************************";
  }

  // Initialize logging.
  // TODO(benh): Replace whitespace in framework.name() with '_'?
  if (flags.initialize_driver_logging) {
    logging::initialize(framework.name(), flags);
  } else {
    VLOG(1) << "Disabling initialization of GLOG logging";
  }

  // Initialize mutex and condition variable. TODO(benh): Consider
  // using a libprocess Latch rather than a pthread mutex and
  // condition variable for signaling.
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);

  // TODO(benh): Check the user the framework wants to run tasks as,
  // see if the current user can switch to that user, or via an
  // authentication module ensure this is acceptable.

  // See FrameWorkInfo in include/mesos/mesos.proto:
  if (framework.user().empty()) {
    Result<string> user = os::user();
    CHECK_SOME(user);

    framework.set_user(user.get());
  }
  if (framework.hostname().empty()) {
    framework.set_hostname(net::hostname().get());
  }

  // Launch a local cluster if necessary.
  Option<UPID> pid;
  if (master == "local") {
    pid = local::launch(flags);
  }

  CHECK(process == NULL);

  url = pid.isSome() ? static_cast<string>(pid.get()) : master;
}


// Implementation of C++ API.
//
// Notes:
//
// (1) Callbacks should be serialized as well as calls into the
//     class. We do the former because the message reads from
//     SchedulerProcess are serialized. We do the latter currently by
//     using locks for certain methods ... but this may change in the
//     future.
//
// (2) There is a variable called state, that represents the current
//     state of the driver and is used to enforce its state transitions.
// TODO(vinod): Deprecate this in favor of the constructor that takes
// the credential.
MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master)
  : detector(NULL),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(NULL),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(true),
    credential(NULL),
    schedulerId("scheduler-" + UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    const Credential& _credential)
  : detector(NULL),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(NULL),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(true),
    credential(new Credential(_credential)),
    schedulerId("scheduler-" + UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    bool _implicitAcknowlegements)
  : detector(NULL),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(NULL),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(NULL),
    schedulerId("scheduler-" + UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    bool _implicitAcknowlegements,
    const Credential& _credential)
  : detector(NULL),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(NULL),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(new Credential(_credential)),
    schedulerId("scheduler-" + UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::~MesosSchedulerDriver()
{
  // We want to make sure the SchedulerProcess has completed so it
  // doesn't try to make calls into us after we are gone. There is an
  // unfortunate deadlock scenario that occurs when we try and wait
  // for a process that we are currently executing within (e.g.,
  // because a callback on 'this' invoked from a SchedulerProcess
  // ultimately invokes this destructor). This deadlock is actually a
  // bug in the client code: provided that the SchedulerProcess class
  // _only_ makes calls into instances of Scheduler, then such a
  // deadlock implies that the destructor got called from within a
  // method of the Scheduler instance that is being destructed! Note
  // that we could add a method to libprocess that told us whether or
  // not this was about to be deadlock, and possibly report this back
  // to the user somehow. It might make sense to try and add some more
  // debug output for the case where we wait indefinitely due to
  // deadlock.
  if (process != NULL) {
    // We call 'terminate()' here to ensure that SchedulerProcess
    // terminates even if the user forgot to call stop/abort on the
    // driver.
    terminate(process);
    wait(process);
    delete process;
  }

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

  if (detector != NULL) {
    delete detector;
  }

  // Check and see if we need to shutdown a local cluster.
  if (master == "local" || master == "localquiet") {
    local::shutdown();
  }
}


Status MesosSchedulerDriver::start()
{
  synchronized (mutex) {
    if (status != DRIVER_NOT_STARTED) {
      return status;
    }

    if (detector == NULL) {
      Try<MasterDetector*> detector_ = MasterDetector::create(url);

      if (detector_.isError()) {
        status = DRIVER_ABORTED;
        string message = "Failed to create a master detector for '" +
        master + "': " + detector_.error();
        scheduler->error(this, message);
        return status;
      }

      // Save the detector so we can delete it later.
      detector = detector_.get();
    }

    // Load scheduler flags.
    internal::scheduler::Flags flags;
    Try<Nothing> load = flags.load("MESOS_");

    if (load.isError()) {
      status = DRIVER_ABORTED;
      scheduler->error(this, load.error());
      return status;
    }

    // Initialize modules. Note that since other subsystems may depend
    // upon modules, we should initialize modules before anything else.
    if (flags.modules.isSome()) {
      Try<Nothing> result = modules::ModuleManager::load(flags.modules.get());
      if (result.isError()) {
        status = DRIVER_ABORTED;
        scheduler->error(this, "Error loading modules: " + result.error());
        return status;
      }
    }

    CHECK(process == NULL);

    if (credential == NULL) {
      process = new SchedulerProcess(
          this,
          scheduler,
          framework,
          None(),
          implicitAcknowlegements,
          schedulerId,
          detector,
          flags,
          &mutex,
          &cond);
    } else {
      const Credential& cred = *credential;
      process = new SchedulerProcess(
          this,
          scheduler,
          framework,
          cred,
          implicitAcknowlegements,
          schedulerId,
          detector,
          flags,
          &mutex,
          &cond);
    }

    spawn(process);

    return status = DRIVER_RUNNING;
  }
}


Status MesosSchedulerDriver::stop(bool failover)
{
  synchronized (mutex) {
    LOG(INFO) << "Asked to stop the driver";

    if (status != DRIVER_RUNNING && status != DRIVER_ABORTED) {
      VLOG(1) << "Ignoring stop because the status of the driver is "
              << Status_Name(status);
      return status;
    }

    // 'process' might be NULL if the driver has failed to instantiate
    // it due to bad parameters (e.g. error in creating the detector
    // or loading flags).
    if (process != NULL) {
      process->running =  false;
      dispatch(process, &SchedulerProcess::stop, failover);
    }

    // TODO(benh): It might make more sense to clean up our local
    // cluster here than in the destructor. However, what would be
    // even better is to allow multiple local clusters to exist (i.e.
    // not use global vars in local.cpp) so that ours can just be an
    // instance variable in MesosSchedulerDriver.

    bool aborted = status == DRIVER_ABORTED;

    status = DRIVER_STOPPED;

    return aborted ? DRIVER_ABORTED : status;
  }
}


Status MesosSchedulerDriver::abort()
{
  synchronized (mutex) {
    LOG(INFO) << "Asked to abort the driver";

    if (status != DRIVER_RUNNING) {
      VLOG(1) << "Ignoring abort because the status of the driver is "
              << Status_Name(status);
      return status;
    }

    CHECK_NOTNULL(process);
    process->running = false;

    // Dispatching here ensures that we still process the outstanding
    // requests *from* the scheduler, since those do proceed when
    // aborted is true.
    dispatch(process, &SchedulerProcess::abort);

    return status = DRIVER_ABORTED;
  }
}


Status MesosSchedulerDriver::join()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    while (status == DRIVER_RUNNING) {
      pthread_cond_wait(&cond, &mutex);
    }

    CHECK(status == DRIVER_ABORTED || status == DRIVER_STOPPED);

    return status;
  }
}


Status MesosSchedulerDriver::run()
{
  Status status = start();
  return status != DRIVER_RUNNING ? status : join();
}


Status MesosSchedulerDriver::killTask(const TaskID& taskId)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::killTask, taskId);

    return status;
  }
}


Status MesosSchedulerDriver::launchTasks(
    const OfferID& offerId,
    const vector<TaskInfo>& tasks,
    const Filters& filters)
{
  vector<OfferID> offerIds;
  offerIds.push_back(offerId);

  return launchTasks(offerIds, tasks, filters);
}


Status MesosSchedulerDriver::launchTasks(
    const vector<OfferID>& offerIds,
    const vector<TaskInfo>& tasks,
    const Filters& filters)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::launchTasks, offerIds, tasks, filters);

    return status;
  }
}


Status MesosSchedulerDriver::acceptOffers(
    const vector<OfferID>& offerIds,
    const vector<Offer::Operation>& operations,
    const Filters& filters)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(
        process,
        &SchedulerProcess::acceptOffers,
        offerIds,
        operations,
        filters);

    return status;
  }
}


Status MesosSchedulerDriver::declineOffer(
    const OfferID& offerId,
    const Filters& filters)
{
  vector<OfferID> offerIds;
  offerIds.push_back(offerId);

  return launchTasks(offerIds, vector<TaskInfo>(), filters);
}


Status MesosSchedulerDriver::reviveOffers()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::reviveOffers);

    return status;
  }
}


Status MesosSchedulerDriver::acknowledgeStatusUpdate(
    const TaskStatus& taskStatus)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    // TODO(bmahler): Should this use abort() instead?
    if (implicitAcknowlegements) {
      ABORT("Cannot call acknowledgeStatusUpdate:"
            " Implicit acknowledgements are enabled");
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::acknowledgeStatusUpdate, taskStatus);

    return status;
  }
}


Status MesosSchedulerDriver::sendFrameworkMessage(
    const ExecutorID& executorId,
    const SlaveID& slaveId,
    const string& data)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::sendFrameworkMessage,
            executorId, slaveId, data);

    return status;
  }
}


Status MesosSchedulerDriver::reconcileTasks(
    const vector<TaskStatus>& statuses)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::reconcileTasks, statuses);

    return status;
  }
}


Status MesosSchedulerDriver::requestResources(
    const vector<Request>& requests)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != NULL);

    dispatch(process, &SchedulerProcess::requestResources, requests);

    return status;
  }
}
