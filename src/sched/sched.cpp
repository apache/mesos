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

#ifndef __WINDOWS__
#include <dlfcn.h>
#endif // __WINDOWS__
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authentication/authenticatee.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/module/authenticatee.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/latch.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <process/metrics/pull_gauge.hpp>
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

#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "sched/constants.hpp"
#include "sched/flags.hpp"

#include "version/version.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;
using namespace mesos::scheduler;

using google::protobuf::RepeatedPtrField;

using mesos::scheduler::OfferConstraints;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::DispatchEvent;
using process::Future;
using process::Latch;
using process::MessageEvent;
using process::Process;
using process::UPID;

using std::map;
using std::make_move_iterator;
using std::mutex;
using std::shared_ptr;
using std::set;
using std::string;
using std::vector;
using std::weak_ptr;

using process::wait; // Necessary on some OS's to disambiguate.

using utils::copy;

namespace mesos {
namespace internal {


// The DetectorPool is responsible for tracking single detector per url
// to avoid having multiple detectors per url when multiple frameworks
// are instantiated per process. See MESOS-3595.

class DetectorPool
{
public:
  virtual ~DetectorPool() {}

  static Try<shared_ptr<MasterDetector>> get(const string& url)
  {
    synchronized (DetectorPool::instance()->poolMutex) {
      // Get or create the `weak_ptr` map entry.
      shared_ptr<MasterDetector> result =
        DetectorPool::instance()->pool[url].lock();

      if (result) {
        // Return existing master detector.
        return result;
      } else {
        // Else, create the master detector and record it in the map.
        Try<MasterDetector*> detector = MasterDetector::create(url);
        if (detector.isError()) {
          return Error(detector.error());
        }

        result = shared_ptr<MasterDetector>(detector.get());
        DetectorPool::instance()->pool[url] = result;
        return result;
      }
    }
  }

private:
  // Hide the constructors and assignment operator.
  DetectorPool() {}
  DetectorPool(const DetectorPool&) = delete;
  DetectorPool& operator=(const DetectorPool&) = delete;

  // Instead of having multiple detectors for multiple frameworks,
  // keep track of one detector per url.
  hashmap<string, weak_ptr<MasterDetector>> pool;
  std::mutex poolMutex;

  // Internal Singleton.
  static DetectorPool* instance()
  {
    static DetectorPool* singleton = new DetectorPool();
    return singleton;
  }
};

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
                   const vector<string>& _suppressedRoles,
                   const Option<Credential>& _credential,
                   bool _implicitAcknowledgements,
                   const string& schedulerId,
                   MasterDetector* _detector,
                   const internal::scheduler::Flags& _flags,
                   std::recursive_mutex* _mutex,
                   Latch* _latch)
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
      suppressedRoles(_suppressedRoles.begin(), _suppressedRoles.end()),
      mutex(_mutex),
      latch(_latch),
      failover(_framework.has_id() && !framework.id().value().empty()),
      connected(false),
      sendUpdateFrameworkOnConnect(false),
      running(true),
      detector(_detector),
      flags(_flags),
      implicitAcknowledgements(_implicitAcknowledgements),
      credential(_credential),
      authenticatee(nullptr),
      authenticating(None()),
      authenticated(false),
      reauthenticate(false),
      failedAuthentications(0)
  {
    LOG(INFO) << "Version: " << MESOS_VERSION;
  }

  ~SchedulerProcess() override
  {
    delete authenticatee;
  }

protected:
  void initialize() override
  {
    install<Event>(&SchedulerProcess::receive);

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

    install<ExitedExecutorMessage>(
        &SchedulerProcess::lostExecutor,
        &ExitedExecutorMessage::executor_id,
        &ExitedExecutorMessage::slave_id,
        &ExitedExecutorMessage::status);

    install<ExecutorToFrameworkMessage>(
        &SchedulerProcess::frameworkMessage,
        &ExecutorToFrameworkMessage::slave_id,
        &ExecutorToFrameworkMessage::executor_id,
        &ExecutorToFrameworkMessage::data);

    install<FrameworkErrorMessage>(
        &SchedulerProcess::error,
        &FrameworkErrorMessage::message);

    // Start detecting masters.
    detector->detect()
      .onAny(defer(self(), &SchedulerProcess::detected, lambda::_1));
  }

  void detected(const Future<Option<MasterInfo>>& _master)
  {
    if (!running.load()) {
      VLOG(1) << "Ignoring the master change because the driver is not"
              << " running!";
      return;
    }

    CHECK(!_master.isDiscarded());

    if (_master.isFailed()) {
      EXIT(EXIT_FAILURE) << "Failed to detect a master: " << _master.failure();
    }

    if (_master->isSome()) {
      master = _master->get();
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
      LOG(INFO) << "New master detected at " << master->pid();
      link(master->pid());

      // Cancel the pending registration timer to avoid spurious attempts
      // at reregistration. `Clock::cancel` is idempotent, so this call
      // is safe even if no timer is active or pending.
      Clock::cancel(frameworkRegistrationTimer);

      if (credential.isSome()) {
        // Authenticate with the master.
        // TODO(adam-mesos): Consider adding an initial delay like we do for
        // slave registration, to combat thundering herds on master failover.
        authenticate(
            flags.authentication_timeout_min,
            std::min(
                flags.authentication_timeout_min +
                  flags.authentication_backoff_factor * 2,
                flags.authentication_timeout_max));
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

  void authenticate(Duration minTimeout, Duration maxTimeout)
  {
    if (!running.load()) {
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

    LOG(INFO) << "Authenticating with master " << master->pid();

    CHECK_SOME(credential);

    CHECK(authenticatee == nullptr);

    if (flags.authenticatee == scheduler::DEFAULT_AUTHENTICATEE) {
      LOG(INFO) << "Using default CRAM-MD5 authenticatee";
      authenticatee = new cram_md5::CRAMMD5Authenticatee();
    } else {
      Try<Authenticatee*> module =
        modules::ModuleManager::create<Authenticatee>(flags.authenticatee);
      if (module.isError()) {
        EXIT(EXIT_FAILURE)
          << "Could not create authenticatee module '"
          << flags.authenticatee << "': " << module.error();
      }
      LOG(INFO) << "Using '" << flags.authenticatee << "' authenticatee";
      authenticatee = module.get();
    }

    // We pick a random duration between `minTimeout` and `maxTimeout`.
    Duration timeout = minTimeout + (maxTimeout - minTimeout) *
                                      ((double)os::random() / RAND_MAX);

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
      authenticatee->authenticate(master->pid(), self(), credential.get())
        .onAny(defer(self(), &Self::_authenticate, minTimeout, maxTimeout))
        .after(timeout, [](Future<bool> future) {
          // NOTE: Discarded future results in a retry in '_authenticate()'.
          // This is a no-op if the future is already ready.
          if (future.discard()) {
            LOG(WARNING) << "Authentication timed out";
          }

          return future;
        });
  }

  void _authenticate(Duration currentMinTimeout, Duration currentMaxTimeout)
  {
    if (!running.load()) {
      VLOG(1) << "Ignoring _authenticate because the driver is not running!";
      return;
    }

    delete CHECK_NOTNULL(authenticatee);
    authenticatee = nullptr;

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
        << "Failed to authenticate with master " << master->pid() << ": "
        << (reauthenticate ? "master changed" :
           (future.isFailed() ? future.failure() : "future discarded"));

      authenticating = None();
      reauthenticate = false;

      // TODO(vinod): Add a limit on number of retries.

      // Grow the timeout range using exponential backoff:
      //
      //   [min, min + factor * 2^0]
      //   [min, min + factor * 2^1]
      //   ...
      //   [min, min + factor * 2^N]
      //   ...
      //   [min, max] // Stop at max.
      Duration maxTimeout =
        currentMinTimeout + (currentMaxTimeout - currentMinTimeout) * 2;

      authenticate(
          currentMinTimeout,
          std::min(maxTimeout, flags.authentication_timeout_max));

      return;
    }

    if (!future.get()) {
      LOG(ERROR) << "Master " << master->pid() << " refused authentication";
      error("Master refused authentication");
      return;
    }

    LOG(INFO) << "Successfully authenticated with master " << master->pid();

    authenticated = true;
    authenticating = None();

    failedAuthentications = 0;

    doReliableRegistration(flags.registration_backoff_factor);
  }

  void drop(const Event& event, const string& message)
  {
    // TODO(bmahler): Increment a metric.

    LOG(WARNING) << "Dropping " << event.type() << ": " << message;
  }

  void receive(const UPID& from, const Event& event)
  {
    switch (event.type()) {
      case Event::SUBSCRIBED: {
        if (!event.has_subscribed()) {
          drop(event, "Expecting 'subscribed' to be present");
          break;
        }

        // The scheduler API requires a MasterInfo be passed during
        // (re-)registration, so we rely on the MasterInfo provided
        // by the detector. If it's None, the driver would have
        // dropped the message.
        if (master.isNone()) {
          drop(event, "No master detected");
          break;
        }

        const FrameworkID& frameworkId = event.subscribed().framework_id();

        // Cancel the pending registration timer to avoid spurious attempts
        // at reregistration. `Clock::cancel` is idempotent, so this call
        // is safe even if no timer is active or pending.
        Clock::cancel(frameworkRegistrationTimer);

        // We match the existing registration semantics of the
        // driver, except for the 3rd case in MESOS-786 (since
        // it requires non-local knowledge and schedulers could
        // not have possibly relied on this case).
        if (!framework.has_id() || framework.id().value().empty()) {
          registered(from, frameworkId, master.get());
        } else if (failover) {
          registered(from, frameworkId, master.get());
        } else {
          reregistered(from, frameworkId, master.get());
        }

        break;
      }

      case Event::OFFERS: {
        if (!event.has_offers()) {
          drop(event, "Expecting 'offers' to be present");
          break;
        }

        const vector<Offer> offers =
          google::protobuf::convert(event.offers().offers());

        vector<string> pids;

        foreach (const Offer& offer, offers) {
          CHECK(offer.has_url())
            << "Offer.url required for Event support";
          CHECK(offer.url().has_path())
            << "Offer.url.path required for Event support";

          string id = offer.url().path();
          id = strings::trim(id, "/");

          Try<net::IP> ip =
            net::IP::parse(offer.url().address().ip(), AF_INET);

          CHECK_SOME(ip) << "Failed to parse Offer.url.address.ip";

          pids.push_back(UPID(id, ip.get(), offer.url().address().port()));
        }

        resourceOffers(from, offers, pids);

        break;
      }

      case Event::RESCIND: {
        if (!event.has_rescind()) {
          drop(event, "Expecting 'rescind' to be present");
          break;
        }

        // TODO(bmahler): Rename 'rescindOffer' to 'rescind'
        // to match the Event naming scheme.
        rescindOffer(from, event.rescind().offer_id());
        break;
      }

      case Event::UPDATE: {
        if (!event.has_update()) {
          drop(event, "Expecting 'update' to be present");
          break;
        }

        const TaskStatus& status = event.update().status();

        // Create a StatusUpdate based on the TaskStatus.
        StatusUpdate update;
        update.mutable_framework_id()->CopyFrom(framework.id());
        update.mutable_status()->CopyFrom(status);
        update.set_timestamp(status.timestamp());

        if (status.has_executor_id()) {
          update.mutable_executor_id()->CopyFrom(status.executor_id());
        }

        if (status.has_slave_id()) {
          update.mutable_slave_id()->CopyFrom(status.slave_id());
        }

        if (status.has_uuid()) {
          update.set_uuid(status.uuid());
        }

        // Note that we do not need to set the 'pid' now that
        // the driver uses 'uuid' absence to skip acknowledgement.
        //
        // TODO(bmahler): Implement an 'update' method to match
        // the Event naming scheme, and have 'statusUpdate' call
        // into it.
        statusUpdate(from, update, UPID());
        break;
      }

      // TODO(greggomann): Implement handling of operation status updates.
      case Event::UPDATE_OPERATION_STATUS:
        break;

      case Event::MESSAGE: {
        if (!event.has_message()) {
          drop(event, "Expecting 'message' to be present");
          break;
        }

        // TODO(bmahler): Rename 'frameworkMessage' to 'message'
        // to match the Event naming scheme.
        frameworkMessage(
            event.message().slave_id(),
            event.message().executor_id(),
            event.message().data());
        break;
      }

      case Event::FAILURE: {
        if (!event.has_failure()) {
          drop(event, "Expecting 'failure' to be present");
          break;
        }

        // TODO(bmahler): Add a 'failure' method and have the
        // lost slave handler call into it.

        if (event.failure().has_slave_id() &&
            event.failure().has_executor_id()) {
          CHECK(event.failure().has_status());
          lostExecutor(
              from,
              event.failure().executor_id(),
              event.failure().slave_id(),
              event.failure().status());
        } else if (event.failure().has_slave_id()) {
          lostSlave(from, event.failure().slave_id());
        } else {
          drop(event, "Expecting 'slave_id' to be present");
        }

        break;
      }

      case Event::ERROR: {
        if (!event.has_error()) {
          drop(event, "Expecting 'error' to be present");
          break;
        }

        error(event.error().message());
        break;
      }

      case Event::INVERSE_OFFERS:
      case Event::RESCIND_INVERSE_OFFER:
      case Event::HEARTBEAT: {
        break;
      }

      case Event::UNKNOWN: {
        drop(event, "Unknown event");
        break;
      }
    }
  }

  void registered(
      const UPID& from,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo)
  {
    if (!running.load()) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is not running!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework registered message because "
              << "the driver is already connected!";
      return;
    }

    if (master.isNone() || from != master->pid()) {
      LOG(WARNING)
        << "Ignoring framework registered message because it was sent "
        << "from '" << from << "' instead of the leading master '"
        << (master.isSome() ? UPID(master->pid()) : UPID()) << "'";
      return;
    }

    LOG(INFO) << "Framework registered with " << frameworkId;

    framework.mutable_id()->MergeFrom(frameworkId);

    connected = true;
    failover = false;

    if (sendUpdateFrameworkOnConnect) {
      sendUpdateFramework();
    }
    sendUpdateFrameworkOnConnect = false;

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
    if (!running.load()) {
      VLOG(1) << "Ignoring framework reregistered message because "
              << "the driver is not running!";
      return;
    }

    if (connected) {
      VLOG(1) << "Ignoring framework reregistered message because "
              << "the driver is already connected!";
      return;
    }

    if (master.isNone() || from != master->pid()) {
      LOG(WARNING)
        << "Ignoring framework reregistered message because it was sent "
        << "from '" << from << "' instead of the leading master '"
        << (master.isSome() ? UPID(master->pid()) : UPID()) << "'";
      return;
    }

    LOG(INFO) << "Framework reregistered with " << frameworkId;

    CHECK(framework.id() == frameworkId);

    connected = true;
    failover = false;

    if (sendUpdateFrameworkOnConnect) {
      sendUpdateFramework();
    }
    sendUpdateFrameworkOnConnect = false;


    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->reregistered(driver, masterInfo);

    VLOG(1) << "Scheduler::reregistered took " << stopwatch.elapsed();
  }

  void doReliableRegistration(Duration maxBackoff)
  {
    if (!running.load()) {
      return;
    }

    if (connected || master.isNone()) {
      return;
    }

    if (credential.isSome() && !authenticated) {
      return;
    }

    VLOG(1) << "Sending SUBSCRIBE call to " << master->pid();

    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(framework);
    *subscribe->mutable_offer_constraints() = offerConstraints;
    *subscribe->mutable_suppressed_roles() = RepeatedPtrField<string>(
        suppressedRoles.begin(), suppressedRoles.end());

    if (framework.has_id() && !framework.id().value().empty()) {
      subscribe->set_force(failover);
      call.mutable_framework_id()->CopyFrom(framework.id());
    }

    send(master->pid(), call);

    // Bound the maximum backoff by 'REGISTRATION_RETRY_INTERVAL_MAX'.
    maxBackoff =
      std::min(maxBackoff, scheduler::REGISTRATION_RETRY_INTERVAL_MAX);

    // If failover timeout is present, bound the maximum backoff
    // by 1/10th of the failover timeout.
    if (framework.has_failover_timeout()) {
      Try<Duration> duration = Duration::create(framework.failover_timeout());
      if (duration.isSome() && duration.get() > Duration::zero()) {
        maxBackoff = std::min(maxBackoff, duration.get() / 10);
      }
    }

    // Determine the delay for next attempt by picking a random
    // duration between 0 and 'maxBackoff'.
    // TODO(vinod): Use random numbers from <random> header.
    Duration delay = maxBackoff * ((double) os::random() / RAND_MAX);

    VLOG(1) << "Will retry registration in " << delay << " if necessary";

    // Backoff.
    frameworkRegistrationTimer = process::delay(
        delay, self(), &Self::doReliableRegistration, maxBackoff * 2);
  }

  void resourceOffers(
      const UPID& from,
      const vector<Offer>& offers,
      const vector<string>& pids)
  {
    if (!running.load()) {
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

    if (from != master->pid()) {
      VLOG(1) << "Ignoring resource offers message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master->pid() << "'";
      return;
    }

    // We exit early if `offers` is empty since we don't implement inverse
    // offers in the old scheduler API. It could be empty when there are only
    // inverse offers as part of the `ResourceOffersMessage`.
    if (offers.empty()) {
      return;
    }

    VLOG(2) << "Received " << offers.size() << " offers";

    CHECK_EQ(offers.size(), pids.size());

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
    if (!running.load()) {
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

    if (from != master->pid()) {
      VLOG(1) << "Ignoring rescind offer message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master->pid() << "'";
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
    if (!running.load()) {
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

      if (from != master->pid()) {
        VLOG(1) << "Ignoring status update message because it was sent "
                << "from '" << from << "' instead of the leading master '"
                << master->pid() << "'";
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
    // TODO(vinod): Get rid of this logic in 0.27.0 because master
    // correctly sets task status since 0.26.0.
    if (!update.has_uuid() || update.uuid() == "") {
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
      // Note that we need to look at the atomic 'running' here
      // so that we don't acknowledge the update if the driver was
      // aborted during the processing of the update.
      if (!running.load()) {
        VLOG(1) << "Not sending status update acknowledgment message because "
                << "the driver is not running!";
        return;
      }

      // See above for when we don't need to acknowledge.
      if ((update.has_uuid() && update.uuid() != "") ||
          (from != UPID() && pid != UPID())) {
        // We drop updates while we're disconnected.
        CHECK(connected);
        CHECK_SOME(master);

        VLOG(2) << "Sending ACK for status update " << update
                << " to " << master->pid();

        Call call;

        CHECK(framework.has_id());
        call.mutable_framework_id()->CopyFrom(framework.id());
        call.set_type(Call::ACKNOWLEDGE);

        Call::Acknowledge* acknowledge = call.mutable_acknowledge();
        acknowledge->mutable_slave_id()->CopyFrom(update.slave_id());
        acknowledge->mutable_task_id()->CopyFrom(update.status().task_id());
        acknowledge->set_uuid(update.uuid());

        CHECK_SOME(master);
        send(master->pid(), call);
      }
    }
  }

  void lostSlave(const UPID& from, const SlaveID& slaveId)
  {
    if (!running.load()) {
      VLOG(1) << "Ignoring lost agent message because the driver is not"
              << " running!";
      return;
    }

    if (!connected) {
      VLOG(1) << "Ignoring lost agent message because the driver is "
              << "disconnected!";
      return;
    }

    CHECK_SOME(master);

    if (from != master->pid()) {
      VLOG(1) << "Ignoring lost agent message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master->pid() << "'";
      return;
    }

    VLOG(1) << "Lost agent " << slaveId;

    savedSlavePids.erase(slaveId);

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->slaveLost(driver, slaveId);

    VLOG(1) << "Scheduler::slaveLost took " << stopwatch.elapsed();
  }

  void lostExecutor(
      const UPID& from,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int32_t status)
  {
    if (!running.load()) {
      VLOG(1)
        << "Ignoring lost executor message because the driver is not running!";
      return;
    }

    if (!connected) {
      VLOG(1)
        << "Ignoring lost executor message because the driver is disconnected!";
      return;
    }

    CHECK_SOME(master);
    if (from != master->pid()) {
      VLOG(1) << "Ignoring lost executor message because it was sent "
              << "from '" << from << "' instead of the leading master '"
              << master->pid() << "'";
      return;
    }

    VLOG(1)
      << "Executor " << executorId << " on agent " << slaveId
      << " exited with status " << status;

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    scheduler->executorLost(driver, executorId, slaveId, status);

    VLOG(1) << "Scheduler::executorLost took " << stopwatch.elapsed();
  }

  void frameworkMessage(
      const SlaveID& slaveId,
      const ExecutorID& executorId,
      const string& data)
  {
    if (!running.load()) {
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
    if (!running.load()) {
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
    LOG(INFO) << "Stopping framework " << framework.id();

    // Whether or not we send an unregister message, we want to
    // terminate this process.
    terminate(self());

    if (connected && !failover) {
      Call call;

      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::TEARDOWN);

      CHECK_SOME(master);
      send(master->pid(), call);
    }

    synchronized (mutex) {
      CHECK_NOTNULL(latch)->trigger();
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
    LOG(INFO) << "Aborting framework " << framework.id();

    CHECK(!running.load());

    if (!connected) {
      VLOG(1) << "Not sending a deactivate message as master is disconnected";
    } else {
      DeactivateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework.id());
      CHECK_SOME(master);
      send(master->pid(), message);
    }

    synchronized (mutex) {
      CHECK_NOTNULL(latch)->trigger();
    }
  }

  void killTask(const TaskID& taskId)
  {
    if (!connected) {
      VLOG(1) << "Ignoring kill task message as master is disconnected";
      return;
    }

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskId);

    CHECK_SOME(master);
    send(master->pid(), call);
  }

  void requestResources(const vector<Request>& requests)
  {
    if (!connected) {
      VLOG(1) << "Ignoring request resources message as master is disconnected";
      return;
    }

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::REQUEST);

    Call::Request* request = call.mutable_request();
    foreach (const Request& _request, requests) {
      request->add_requests()->CopyFrom(_request);
    }

    CHECK_SOME(master);
    send(master->pid(), call);
  }

  void launchTasks(const vector<OfferID>& offerIds,
                   const vector<TaskInfo>& tasks,
                   const Filters& filters)
  {
    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH);

    Offer::Operation::Launch* launch = operation.mutable_launch();
    foreach (const TaskInfo& task, tasks) {
      launch->add_task_infos()->CopyFrom(task);
    }

    acceptOffers(offerIds, {operation}, filters);
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

      // Reply to the framework with TASK_DROPPED messages for each
      // task launch. If the framework is not partition-aware, we send
      // TASK_LOST instead. See details from notes in `launchTasks`.
      TaskState newTaskState = TASK_DROPPED;
      if (!protobuf::frameworkHasCapability(
              framework, FrameworkInfo::Capability::PARTITION_AWARE)) {
        newTaskState = TASK_LOST;
      }

      foreach (const Offer::Operation& operation, operations) {
        if (operation.type() != Offer::Operation::LAUNCH) {
          continue;
        }

        foreach (const TaskInfo& task, operation.launch().task_infos()) {
          StatusUpdate update = protobuf::createStatusUpdate(
              framework.id(),
              None(),
              task.task_id(),
              newTaskState,
              TaskStatus::SOURCE_MASTER,
              None(),
              "Master disconnected",
              TaskStatus::REASON_MASTER_DISCONNECTED);

          statusUpdate(UPID(), update, UPID());
        }
      }
      return;
    }

    Call call;
    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();

    // Setting accept.operations.
    foreach (const Offer::Operation& _operation, operations) {
      if (_operation.has_id()) {
        ABORT("An offer operation's 'id' field was set, which is disallowed"
              " because the SchedulerDriver cannot handle offer operation"
              " status updates");
      }

      Offer::Operation* operation = accept->add_operations();
      operation->CopyFrom(_operation);
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
                           << " with the wrong agent id " << slaveId;
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
    send(master->pid(), call);
  }

  void declineOffer(
      const OfferID& offerId,
      const Filters& filters)
  {
    if (!connected) {
      VLOG(1) << "Ignoring decline offer message as master is disconnected";
      return;
    }

    if (!savedOffers.contains(offerId)) {
      LOG(WARNING) << "Attempting to decline an unknown offer " << offerId;
    }

    // Remove the offer. We do not need to save any PIDs
    // when declining an offer.
    savedOffers.erase(offerId);

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(offerId);
    decline->mutable_filters()->CopyFrom(filters);

    CHECK_SOME(master);
    send(master->pid(), call);
  }

  void reviveOffers(const vector<string>& roles)
  {
    if (roles.empty()) {
      suppressedRoles.clear();
    } else {
      for (const string& role : roles) {
        suppressedRoles.erase(role);
      }
    }

    if (!connected) {
      VLOG(1) << "Ignoring REVIVE as master is disconnected;"
              << " the set of suppressed roles in the driver has been updated"
              << " and will be sent to the master during re-registration";
      sendUpdateFrameworkOnConnect = true;
      return;
    }

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::REVIVE);

    if (roles.empty()) {
      VLOG(2) << "Sending REVIVE for all roles";
    } else {
      VLOG(2) << "Sending REVIVE for roles: " << stringify(roles);
      *call.mutable_revive()->mutable_roles() =
        RepeatedPtrField<string>(roles.begin(), roles.end());
    }

    CHECK_SOME(master);
    send(master->pid(), call);
  }

  void suppressOffers(const vector<string>& roles)
  {
    if (roles.empty()) {
      suppressedRoles =
        std::set<string>(framework.roles().begin(), framework.roles().end());
    } else {
      for (const string& role : roles) {
        suppressedRoles.emplace(role);
      }
    }

    if (!connected) {
      VLOG(1) << "Ignoring SUPPRESS as master is disconnected;"
              << " the set of suppressed roles in the driver has been updated"
              << " and will be sent to the master during re-registration";

      sendUpdateFrameworkOnConnect = true;
      return;
    }

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::SUPPRESS);

    if (roles.empty()) {
      VLOG(2) << "Sending SUPPRESS for all roles";
    } else {
      VLOG(2) << "Sending SUPPRESS for roles: " << stringify(roles);
      *call.mutable_suppress()->mutable_roles() =
        RepeatedPtrField<string>(roles.begin(), roles.end());
    }

    CHECK_SOME(master);
    send(master->pid(), call);
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

    // NOTE: By ignoring the atomic 'running' here, we ensure that all
    // acknowledgements requested before the driver was stopped or
    // aborted are processed. Any acknowledgement that is requested
    // after the driver stops or aborts (running.load() == false) will
    // be dropped in the driver before reaching here.

    // Only statuses with a 'uuid' and a 'slave_id' need to have
    // acknowledgements sent to the master. Note that the driver
    // ensures that master-generated and driver-generated updates
    // will not have a 'uuid' set.
    if (status.has_uuid() && status.has_slave_id()) {
      CHECK_SOME(master);

      VLOG(2) << "Sending ACK for status update " << status.uuid()
              << " of task " << status.task_id()
              << " on agent " << status.slave_id()
              << " to " << master->pid();

      Call call;

      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::ACKNOWLEDGE);

      Call::Acknowledge* acknowledge = call.mutable_acknowledge();
      acknowledge->mutable_slave_id()->CopyFrom(status.slave_id());
      acknowledge->mutable_task_id()->CopyFrom(status.task_id());
      acknowledge->set_uuid(status.uuid());

      send(master->pid(), call);
    } else {
      VLOG(2) << "Received ACK for status update"
              << (status.has_uuid() ? " " + status.uuid() : "")
              << " of task " << status.task_id()
              << (status.has_slave_id()
                  ? " on agent " + stringify(status.slave_id()) : "");
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

    VLOG(2) << "Asked to send framework message to agent "
            << slaveId;

    // TODO(benh): After a scheduler has reregistered it won't have
    // any saved slave PIDs, maybe it makes sense to try and save each
    // PID that this scheduler tries to send a message to? Or we can
    // just wait for them to recollect as new offers come in and get
    // accepted.

    if (savedSlavePids.count(slaveId) > 0) {
      UPID slave = savedSlavePids[slaveId];
      CHECK(slave != UPID());

      // TODO(vinod): Send a Call directly to the slave once that
      // support is added.
      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(framework.id());
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave, message);
    } else {
      VLOG(1) << "Cannot send directly to agent " << slaveId
              << "; sending through master";

      Call call;

      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::MESSAGE);

      Call::Message* message = call.mutable_message();
      message->mutable_slave_id()->CopyFrom(slaveId);
      message->mutable_executor_id()->CopyFrom(executorId);
      message->set_data(data);

      CHECK_SOME(master);
      send(master->pid(), call);
    }
  }

  void reconcileTasks(const vector<TaskStatus>& statuses)
  {
    if (!connected) {
     VLOG(1) << "Ignoring task reconciliation as master is disconnected";
     return;
    }

    Call call;

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::RECONCILE);

    Call::Reconcile* reconcile = call.mutable_reconcile();

    foreach (const TaskStatus& status, statuses) {
      Call::Reconcile::Task* task = reconcile->add_tasks();
      task->mutable_task_id()->CopyFrom(status.task_id());
      if (status.has_slave_id()) {
        task->mutable_slave_id()->CopyFrom(status.slave_id());
      }
    }

    CHECK_SOME(master);
    send(master->pid(), call);
  }

  void updateFramework(
      const FrameworkInfo& framework_,
      set<string>&& suppressedRoles_,
      OfferConstraints&& offerConstraints_)
  {
    if (!framework.has_id() || framework.id().value().empty()) {
      error("MesosSchedulerDriver::updateFramework() must not be called"
            " prior to registration with the master");
      return;
    }

    if (framework_.id() != framework.id()) {
      error("The 'FrameworkInfo.id' provided to"
            " MesosSchedulerDriver::updateFramework()"
            " (" + stringify(framework_.id()) + ")"
            " must be equal to the value known to the MesosSchedulerDriver"
            " (" + stringify(framework.id()) + ")");
      return;
    }

    framework = framework_;
    suppressedRoles = std::move(suppressedRoles_);
    offerConstraints = std::move(offerConstraints_);

    if (connected) {
      sendUpdateFramework();
    } else {
      VLOG(1) << "Postponing UPDATE_FRAMEWORK call:"
                 " not registered with master";
      sendUpdateFrameworkOnConnect = true;
    }
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
    process::metrics::PullGauge event_queue_messages;
    process::metrics::PullGauge event_queue_dispatches;
  } metrics;

  double _event_queue_messages()
  {
    return static_cast<double>(eventCount<MessageEvent>());
  }

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<DispatchEvent>());
  }

  void sendUpdateFramework()
  {
    Call call;

    CHECK(framework.has_id());
    *call.mutable_framework_id() = framework.id();

    call.set_type(Call::UPDATE_FRAMEWORK);
    Call::UpdateFramework* updateFramework = call.mutable_update_framework();
    *updateFramework->mutable_framework_info() = framework;
    *updateFramework->mutable_offer_constraints() = offerConstraints;
    *updateFramework->mutable_suppressed_roles() =
      RepeatedPtrField<string>(suppressedRoles.begin(), suppressedRoles.end());

    VLOG(1) << "Sending UPDATE_FRAMEWORK message";

    CHECK_SOME(master);
    send(master->pid(), call);
  }


  MesosSchedulerDriver* driver;
  Scheduler* scheduler;
  FrameworkInfo framework;
  set<string> suppressedRoles;
  OfferConstraints offerConstraints;

  std::recursive_mutex* mutex;
  Latch* latch;

  bool failover;

  Option<MasterInfo> master;

  bool connected; // Flag to indicate if framework is registered.

  // Flag to indicate that an UPDATE_FRAMEWORK with a current FrameworkInfo
  // should be sent after successful (re)connection attempt.
  bool sendUpdateFrameworkOnConnect;

  // TODO(vinod): Instead of 'bool' use 'Status'.
  // We set 'running' to false in SchedulerDriver::stop() and
  // SchedulerDriver::abort() to prevent any further messages from
  // being processed in the SchedulerProcess. However, if abort()
  // or stop() is called from a thread other than SchedulerProcess,
  // there may be one additional callback delivered to the scheduler.
  // This could happen if the SchedulerProcess is in the middle of
  // processing an event.
  std::atomic_bool running; // Flag to indicate if the driver is running.

  MasterDetector* detector;

  const internal::scheduler::Flags flags;

  // Timer for triggering registration of the framework with the master.
  process::Timer frameworkRegistrationTimer;

  hashmap<OfferID, hashmap<SlaveID, UPID>> savedOffers;
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
  Option<Future<bool>> authenticating;

  // Indicates if the authentication is successful.
  bool authenticated;

  // Indicates if a new authentication attempt should be enforced.
  bool reauthenticate;

  // Indicates the number of failed authentication attempts.
  uint64_t failedAuthentications;
};

} // namespace internal {
} // namespace mesos {


void fillMissingFrameworkInfoFields(FrameworkInfo* framework)
{
  if (framework->user().empty()) {
    Result<string> user = os::user();
    CHECK_SOME(user);

    *framework->mutable_user() = user.get();
  }

  if (framework->hostname().empty()) {
    Try<string> hostname = net::hostname();

    if (hostname.isSome()) {
      *framework->mutable_hostname() = hostname.get();
    }
  }
}


void MesosSchedulerDriver::initialize() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Load any flags from the environment (we use local::Flags in the
  // event we run in 'local' mode, since it inherits logging::Flags).
  // In the future, just as the TODO in local/main.cpp discusses,
  // we'll probably want a way to load master::Flags and slave::Flags
  // as well.
  local::Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_");

  if (load.isError()) {
    status = DRIVER_ABORTED;
    scheduler->error(this, load.error());
    return;
  }

  // Initialize libprocess. NOTE: We need to ensure this happens
  // before we invoke anything in libprocess. While libprocess will
  // call `process::initialize` internally if needed, it will do so
  // without passing any parameters; any subsequent calls to
  // `process::initialize` (with non-empty arguments) will be ignored.
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
    logging::initialize(framework.name(), false, flags);
  } else {
    VLOG(1) << "Disabling initialization of GLOG logging";
  }

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  spawn(new VersionProcess(), true);

  // Initialize Latch.
  latch = new Latch();

  // TODO(benh): Check the user the framework wants to run tasks as,
  // see if the current user can switch to that user, or via an
  // authentication module ensure this is acceptable.

  fillMissingFrameworkInfoFields(&framework);

  // Launch a local cluster if necessary.
  Option<UPID> pid;
  if (master == "local") {
    pid = local::launch(flags);
  }

  CHECK(process == nullptr);

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
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(true),
    credential(nullptr),
    schedulerId("scheduler-" + id::UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    const Credential& _credential)
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(true),
    credential(new Credential(_credential)),
    schedulerId("scheduler-" + id::UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    bool _implicitAcknowlegements)
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(nullptr),
    schedulerId("scheduler-" + id::UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const string& _master,
    bool _implicitAcknowlegements,
    const Credential& _credential)
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(new Credential(_credential)),
    schedulerId("scheduler-" + id::UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const vector<string>& _suppressedRoles,
    const string& _master,
    bool _implicitAcknowlegements)
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    initialSuppressedRoles(_suppressedRoles),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(nullptr),
    schedulerId("scheduler-" + id::UUID::random().toString())
{
  initialize();
}


MesosSchedulerDriver::MesosSchedulerDriver(
    Scheduler* _scheduler,
    const FrameworkInfo& _framework,
    const vector<string>& _suppressedRoles,
    const string& _master,
    bool _implicitAcknowlegements,
    const Credential& _credential)
  : detector(nullptr),
    scheduler(_scheduler),
    framework(_framework),
    initialSuppressedRoles(_suppressedRoles),
    master(_master),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    implicitAcknowlegements(_implicitAcknowlegements),
    credential(new Credential(_credential)),
    schedulerId("scheduler-" + id::UUID::random().toString())
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
  if (process != nullptr) {
    // We call 'terminate()' here to ensure that SchedulerProcess
    // terminates even if the user forgot to call stop/abort on the
    // driver.
    terminate(process);
    wait(process);
  }

  delete process;
  delete latch;
  delete credential;

  detector.reset();

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

    if (detector == nullptr) {
      Try<shared_ptr<MasterDetector>> detector_ = DetectorPool::get(url);

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
    Try<flags::Warnings> load = flags.load("MESOS_");

    if (load.isError()) {
      status = DRIVER_ABORTED;
      scheduler->error(this, load.error());
      return status;
    }

    // Log any flag warnings.
    foreach (const flags::Warning& warning, load->warnings) {
      LOG(WARNING) << warning.message;
    }

    // Initialize modules. Note that since other subsystems may depend
    // upon modules, we should initialize modules before anything else.
    if (flags.modules.isSome() && flags.modulesDir.isSome()) {
      status = DRIVER_ABORTED;
      scheduler->error(
          this,
          "Only one of MESOS_MODULES or MESOS_MODULES_DIR should be specified");
      return status;
    }

    if (flags.modulesDir.isSome()) {
      Try<Nothing> result =
        modules::ModuleManager::load(flags.modulesDir.get());
      if (result.isError()) {
        status = DRIVER_ABORTED;
        scheduler->error(this, "Error loading modules: " + result.error());
        return status;
      }
    }

    if (flags.modules.isSome()) {
      Try<Nothing> result = modules::ModuleManager::load(flags.modules.get());
      if (result.isError()) {
        status = DRIVER_ABORTED;
        scheduler->error(this, "Error loading modules: " + result.error());
        return status;
      }
    }

    CHECK(process == nullptr);

    Option<Credential> cred = None();
    if (credential != nullptr) {
      cred = *credential;
    }

    process = new SchedulerProcess(
        this,
        scheduler,
        framework,
        initialSuppressedRoles,
        cred,
        implicitAcknowlegements,
        schedulerId,
        detector.get(),
        flags,
        &mutex,
        latch);

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

    // 'process' might be nullptr if the driver has failed to instantiate
    // it due to bad parameters (e.g. error in creating the detector
    // or loading flags).
    if (process != nullptr) {
      process->running.store(false);
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
    process->running.store(false);

    // Dispatching here ensures that we still process the outstanding
    // requests *from* the scheduler, since those do proceed when
    // aborted is true.
    dispatch(process, &SchedulerProcess::abort);

    return status = DRIVER_ABORTED;
  }
}


Status MesosSchedulerDriver::join()
{
  // We can use the process pointer to detect if the driver was ever
  // started properly. If it wasn't, we return the current status
  // (which should either be DRIVER_NOT_STARTED or DRIVER_ABORTED).
  synchronized (mutex) {
    if (process == nullptr) {
      CHECK(status == DRIVER_NOT_STARTED || status == DRIVER_ABORTED);

      return status;
    }
  }

  // Otherwise, wait for stop() or abort() to trigger the latch.
  CHECK_NOTNULL(latch)->await();

  synchronized (mutex) {
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

    CHECK(process != nullptr);

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

    CHECK(process != nullptr);

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

    CHECK(process != nullptr);

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
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(
        process,
        &SchedulerProcess::declineOffer,
        offerId,
        filters);

    return status;
  }
}


Status MesosSchedulerDriver::reviveOffers()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::reviveOffers, vector<string>());

    return status;
  }
}


Status MesosSchedulerDriver::reviveOffers(const vector<string>& roles)
{
  if (roles.empty()) {
    return status;
  }

  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::reviveOffers, roles);

    return status;
  }
}


Status MesosSchedulerDriver::suppressOffers()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::suppressOffers, vector<string>());

    return status;
  }
}


Status MesosSchedulerDriver::suppressOffers(const vector<string>& roles)
{
  if (roles.empty()) {
    return status;
  }

  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::suppressOffers, roles);

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

    CHECK(process != nullptr);

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

    CHECK(process != nullptr);

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

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::reconcileTasks, statuses);

    return status;
  }
}


Status MesosSchedulerDriver::updateFramework(
  const FrameworkInfo& update,
  const vector<string>& suppressedRoles_,
  OfferConstraints&& offerConstraints_)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    framework = update;

    fillMissingFrameworkInfoFields(&framework);

    CHECK(process != nullptr);

    set<string> suppressedRoles(
        suppressedRoles_.begin(), suppressedRoles_.end());

    CHECK_EQ(suppressedRoles_.size(), suppressedRoles.size())
      << "Invalid suppressed role list: contains"
      << " " << suppressedRoles_.size() - suppressedRoles.size()
      << " duplicates " << suppressedRoles_;

    dispatch(
        process,
        &SchedulerProcess::updateFramework,
        framework,
        std::move(suppressedRoles),
        std::move(offerConstraints_));

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

    CHECK(process != nullptr);

    dispatch(process, &SchedulerProcess::requestResources, requests);

    return status;
  }
}
