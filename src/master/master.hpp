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

#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <stdint.h>

#include <list>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/resources.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/cache.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/memory.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>

#include "common/type_utils.hpp"

#include "files/files.hpp"

#include "master/constants.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/flags.hpp"
#include "master/registrar.hpp"

#include "messages/messages.hpp"

namespace process {
class RateLimiter; // Forward declaration.
}

namespace mesos {
namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

namespace sasl {
class Authenticator;
}

class Authorizer;

namespace master {

// Forward declarations.
namespace allocator {
class Allocator;
}

class Repairer;
class SlaveObserver;
class WhitelistWatcher;

struct Framework;
struct OfferVisitor;
struct Role;
struct Slave;
struct TaskInfoVisitor;

class Master : public ProtobufProcess<Master>
{
public:
  Master(allocator::Allocator* allocator,
         Registrar* registrar,
         Repairer* repairer,
         Files* files,
         MasterContender* contender,
         MasterDetector* detector,
         const Option<Authorizer*>& authorizer,
         const Flags& flags = Flags());

  virtual ~Master();

  void submitScheduler(
      const std::string& name);
  void registerFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo);
  void reregisterFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool failover);
  void unregisterFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void deactivateFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void resourceRequest(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);
  void launchTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskInfo>& tasks,
      const Filters& filters,
      const std::vector<OfferID>& offerIds);
  void reviveOffers(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void schedulerMessage(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);
  void registerSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo);
  void reregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks);

  void unregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId);

  void statusUpdate(
      const StatusUpdate& update,
      const process::UPID& pid);

  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);

  void shutdownSlave(
      const SlaveID& slaveId,
      const std::string& message);

  // TODO(bmahler): It would be preferred to use a unique libprocess
  // Process identifier (PID is not sufficient) for identifying the
  // framework instance, rather than relying on re-registration time.
  void frameworkFailoverTimeout(
      const FrameworkID& frameworkId,
      const process::Time& reregisteredTime);

  void offer(
      const FrameworkID& framework,
      const hashmap<SlaveID, Resources>& resources);

  void reconcileTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskStatus>& statuses);

  void authenticate(
      const process::UPID& from,
      const process::UPID& pid);

  // Invoked when there is a newly elected leading master.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo> >& pid);

  // Invoked when the contender has lost the candidacy.
  // Made public for testing purposes.
  void lostCandidacy(const process::Future<Nothing>& lost);

  // Continuation of recover().
  // Made public for testing purposes.
  process::Future<Nothing> _recover(const Registry& registry);

  // Continuation of reregisterSlave().
  // Made public for testing purposes.
  // TODO(vinod): Instead of doing this create and use a
  // MockRegistrar.
  // TODO(dhamon): Consider FRIEND_TEST macro from gtest.
  void _reregisterSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const process::Future<bool>& readmit);

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const process::UPID& pid);
  virtual void visit(const process::MessageEvent& event);
  virtual void visit(const process::ExitedEvent& event);

  // Invoked when the message is ready to be executed after
  // being throttled.
  // 'principal' being None indicates it is throttled by
  // 'defaultLimiter'.
  void throttled(
      const process::MessageEvent& event,
      const Option<std::string>& principal);

  // Continuations of visit().
  void _visit(const process::MessageEvent& event);
  void _visit(const process::ExitedEvent& event);

  // Helper method invoked when the capacity for a framework
  // principal is exceeded.
  void exceededCapacity(
      const process::MessageEvent& event,
      const Option<std::string>& principal,
      uint64_t capacity);

  // Recovers state from the registrar.
  process::Future<Nothing> recover();
  void recoveredSlavesTimeout(const Registry& registry);

  void _registerSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const process::Future<bool>& admit);

  void __reregisterSlave(
      Slave* slave,
      const std::vector<Task>& tasks);

  // 'promise' is used to signal finish of authentication.
  // 'future' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Owned<process::Promise<Nothing> >& promise,
      const process::Future<Option<std::string> >& future);

  void authenticationTimeout(process::Future<Option<std::string> > future);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  // Return connected frameworks that are not in the process of being removed
  std::vector<Framework*> getActiveFrameworks() const;

  // Invoked when the contender has entered the contest.
  void contended(const process::Future<process::Future<Nothing> >& candidacy);

  // Reconciles a re-registering slave's tasks / executors and sends
  // TASK_LOST updates for tasks known to the master but unknown to
  // the slave.
  void reconcile(
      Slave* slave,
      const std::vector<ExecutorInfo>& executors,
      const std::vector<Task>& tasks);

  // 'registerFramework()' continuation.
  void _registerFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const process::Future<Option<Error> >& validationError);

  // 'reregisterFramework()' continuation.
  void _reregisterFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool failover,
      const process::Future<Option<Error> >& validationError);

  // Add a framework.
  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., remove its tasks and
  // executors and recover the resources.
  void removeFramework(Slave* slave, Framework* framework);

  void deactivate(Framework* framework);
  void disconnect(Slave* slave);

  // Add a slave.
  void addSlave(Slave* slave, bool reregister = false);

  void readdSlave(
      Slave* slave,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks);

  // Remove the slave from the registrar and from the master's state.
  void removeSlave(Slave* slave);
  void _removeSlave(
      const SlaveInfo& slaveInfo,
      const std::vector<StatusUpdate>& updates,
      const process::Future<bool>& removed);

  // Validates the task including authorization.
  // Returns None if the task is valid.
  // Returns Error if the task is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error> > validateTask(
      const TaskInfo& task,
      Framework* framework,
      Slave* slave,
      const Resources& totalResources);

  // Launch a task from a task description.
  void launchTask(const TaskInfo& task, Framework* framework, Slave* slave);

  // 'launchTasks()' continuation.
  void _launchTasks(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<TaskInfo>& tasks,
      const Resources& totalResources,
      const Filters& filters,
      const process::Future<std::list<process::Future<Option<Error> > > >& f);

  // Remove a task.
  void removeTask(Task* task);

  // Forwards the update to the framework.
  void forward(
      const StatusUpdate& update,
      const process::UPID& acknowledgee,
      Framework* framework);

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  Framework* getFramework(const FrameworkID& frameworkId);
  Slave* getSlave(const SlaveID& slaveId);
  Offer* getOffer(const OfferID& offerId);

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

  Option<Credentials> credentials;

private:
  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master) {}

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request);

    // /master/observe
    process::Future<process::http::Response> observe(
        const process::http::Request& request);

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request);

    // /master/roles.json
    process::Future<process::http::Response> roles(
        const process::http::Request& request);

    // /master/shutdown
    process::Future<process::http::Response> shutdown(
        const process::http::Request& request);

    // /master/state.json
    process::Future<process::http::Response> state(
        const process::http::Request& request);

    // /master/stats.json
    process::Future<process::http::Response> stats(
        const process::http::Request& request);

    // /master/tasks.json
    process::Future<process::http::Response> tasks(
        const process::http::Request& request);

    const static std::string HEALTH_HELP;
    const static std::string OBSERVE_HELP;
    const static std::string REDIRECT_HELP;
    const static std::string SHUTDOWN_HELP;
    const static std::string TASKS_HELP;

  private:
    // Helper for doing authentication, returns the credential used if
    // the authentication was successful (or none if no credentials
    // have been given to the master), otherwise an Error.
    Result<Credential> authenticate(const process::http::Request& request);

    // Continuations.
    process::Future<process::http::Response> _shutdown(
        const FrameworkID& id,
        bool authorized = true);

    Master* master;
  } http;

  Master(const Master&);              // No copying.
  Master& operator = (const Master&); // No assigning.

  friend struct OfferVisitor;

  const Flags flags;

  Option<MasterInfo> leader; // Current leading master.

  // Whether we are the current leading master.
  bool elected() const
  {
    return leader.isSome() && leader.get() == info_;
  }

  allocator::Allocator* allocator;
  WhitelistWatcher* whitelistWatcher;
  Registrar* registrar;
  Repairer* repairer;
  Files* files;

  MasterContender* contender;
  MasterDetector* detector;

  const Option<Authorizer*> authorizer;

  MasterInfo info_;

  // Indicates when recovery is complete. Recovery begins once the
  // master is elected as a leader.
  Option<process::Future<Nothing> > recovered;

  struct Slaves
  {
    Slaves() : removed(MAX_REMOVED_SLAVES) {}

    // Imposes a time limit for slaves that we recover from the
    // registry to re-register with the master.
    Option<process::Timer> recoveredTimer;

    // Slaves that have been recovered from the registrar but have yet
    // to re-register. We keep a "reregistrationTimer" above to ensure
    // we remove these slaves if they do not re-register.
    hashset<SlaveID> recovered;

    // Slaves that are in the process of registering.
    hashset<process::UPID> registering;

    // Only those slaves that are re-registering for the first time
    // with this master. We must not answer questions related to
    // these slaves until the registrar determines their fate.
    hashset<SlaveID> reregistering;

    hashmap<SlaveID, Slave*> registered;

    // Slaves that are in the process of being removed from the
    // registrar. Think of these as being partially removed: we must
    // not answer questions related to these until they are removed
    // from the registry.
    hashset<SlaveID> removing;

    // We track removed slaves to preserve the consistency
    // semantics of the pre-registrar code when a non-strict registrar
    // is being used. That is, if we remove a slave, we must make
    // an effort to prevent it from (re-)registering, sending updates,
    // etc. We keep a cache here to prevent this from growing in an
    // unbounded manner.
    // TODO(bmahler): Ideally we could use a cache with set semantics.
    Cache<SlaveID, Nothing> removed;

    bool transitioning(const Option<SlaveID>& slaveId)
    {
      if (slaveId.isSome()) {
        return recovered.contains(slaveId.get()) ||
               reregistering.contains(slaveId.get()) ||
               removing.contains(slaveId.get());
      } else {
        return !recovered.empty() ||
               !reregistering.empty() ||
               !removing.empty();
      }
    }
  } slaves;

  struct Frameworks
  {
    Frameworks() : completed(MAX_COMPLETED_FRAMEWORKS) {}

    hashmap<FrameworkID, Framework*> registered;
    boost::circular_buffer<memory::shared_ptr<Framework> > completed;

    // Principals of frameworks keyed by PID.
    // NOTE: Multiple PIDs can map to the same principal. The
    // principal is None when the framework doesn't specify it.
    // The differences between this map and 'authenticated' are:
    // 1) This map only includes *registered* frameworks. The mapping
    //    is added when a framework (re-)registers.
    // 2) This map includes unauthenticated frameworks (when Master
    //    allows them) if they have principals specified in
    //    FrameworkInfo.
    hashmap<process::UPID, Option<std::string> > principals;
  } frameworks;

  hashmap<OfferID, Offer*> offers;

  hashmap<std::string, Role*> roles;

  // Frameworks/slaves that are currently in the process of authentication.
  // 'authenticating' future for an authenticatee is ready when it is
  // authenticated.
  hashmap<process::UPID, process::Future<Nothing> > authenticating;

  hashmap<process::UPID, process::Owned<sasl::Authenticator> > authenticators;

  // Principals of authenticated frameworks/slaves keyed by PID.
  hashmap<process::UPID, std::string> authenticated;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  // TODO(bmahler): These are deprecated! Please use metrics instead.
  // Statistics (initialized in Master::initialize).
  struct
  {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  struct Metrics
  {
    Metrics(const Master& master);

    ~Metrics();

    process::metrics::Gauge uptime_secs;
    process::metrics::Gauge elected;

    process::metrics::Gauge slaves_active;
    process::metrics::Gauge slaves_inactive;

    process::metrics::Gauge frameworks_active;
    process::metrics::Gauge frameworks_inactive;

    process::metrics::Gauge outstanding_offers;

    // Task state metrics.
    process::metrics::Gauge tasks_staging;
    process::metrics::Gauge tasks_starting;
    process::metrics::Gauge tasks_running;
    process::metrics::Counter tasks_finished;
    process::metrics::Counter tasks_failed;
    process::metrics::Counter tasks_killed;
    process::metrics::Counter tasks_lost;

    // Message counters.
    process::metrics::Counter dropped_messages;

    // Metrics specific to frameworks of a common principal.
    // These metrics have names prefixed by "frameworks/<principal>/".
    struct Frameworks
    {
      // Counters for messages from all frameworks of this principal.
      // Note: We only count messages from active scheduler
      // *instances* while they are *registered*. i.e., messages
      // prior to the completion of (re)registration
      // (AuthenticateMessage and (Re)RegisterFrameworkMessage) and
      // messages from an inactive scheduler instance (after the
      // framework has failed over) are not counted.

      // Framework messages received (before processing).
      process::metrics::Counter messages_received;

      // Framework messages processed.
      // NOTE: This doesn't include dropped messages. Processing of
      // a message may be throttled by a RateLimiter if one is
      // configured for this principal. Also due to Master's
      // asynchronous nature, this doesn't necessarily mean the work
      // requested by this message has finished.
      process::metrics::Counter messages_processed;

      explicit Frameworks(const std::string& principal)
        : messages_received("frameworks/" + principal + "/messages_received"),
          messages_processed("frameworks/" + principal + "/messages_processed")
      {
        process::metrics::add(messages_received);
        process::metrics::add(messages_processed);
      }

      ~Frameworks()
      {
        process::metrics::remove(messages_received);
        process::metrics::remove(messages_processed);
      }
    };

    // Per-framework-principal metrics keyed by the framework
    // principal.
    hashmap<std::string, process::Owned<Frameworks> > frameworks;

    // Messages from schedulers.
    process::metrics::Counter messages_register_framework;
    process::metrics::Counter messages_reregister_framework;
    process::metrics::Counter messages_unregister_framework;
    process::metrics::Counter messages_deactivate_framework;
    process::metrics::Counter messages_kill_task;
    process::metrics::Counter messages_status_update_acknowledgement;
    process::metrics::Counter messages_resource_request;
    process::metrics::Counter messages_launch_tasks;
    process::metrics::Counter messages_revive_offers;
    process::metrics::Counter messages_reconcile_tasks;
    process::metrics::Counter messages_framework_to_executor;

    // Messages from slaves.
    process::metrics::Counter messages_register_slave;
    process::metrics::Counter messages_reregister_slave;
    process::metrics::Counter messages_unregister_slave;
    process::metrics::Counter messages_status_update;
    process::metrics::Counter messages_exited_executor;

    // Messages from both schedulers and slaves.
    process::metrics::Counter messages_authenticate;

    process::metrics::Counter valid_framework_to_executor_messages;
    process::metrics::Counter invalid_framework_to_executor_messages;

    process::metrics::Counter valid_status_updates;
    process::metrics::Counter invalid_status_updates;

    process::metrics::Counter valid_status_update_acknowledgements;
    process::metrics::Counter invalid_status_update_acknowledgements;

    // Recovery counters.
    process::metrics::Counter recovery_slave_removals;

    // Process metrics.
    process::metrics::Gauge event_queue_messages;
    process::metrics::Gauge event_queue_dispatches;
    process::metrics::Gauge event_queue_http_requests;

    // Successful registry operations.
    process::metrics::Counter slave_registrations;
    process::metrics::Counter slave_reregistrations;
    process::metrics::Counter slave_removals;

    // Resource metrics.
    std::vector<process::metrics::Gauge> resources_total;
    std::vector<process::metrics::Gauge> resources_used;
    std::vector<process::metrics::Gauge> resources_percent;
  } metrics;

  // Gauge handlers.
  double _uptime_secs()
  {
    return (process::Clock::now() - startTime).secs();
  }

  double _elected()
  {
    return elected() ? 1 : 0;
  }

  double _slaves_active();

  double _slaves_inactive();

  double _frameworks_active()
  {
    return getActiveFrameworks().size();
  }

  double _frameworks_inactive()
  {
    return frameworks.registered.size() - _frameworks_active();
  }

  double _outstanding_offers()
  {
    return offers.size();
  }

  double _event_queue_messages()
  {
    return static_cast<double>(eventCount<process::MessageEvent>());
  }

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<process::DispatchEvent>());
  }

  double _event_queue_http_requests()
  {
    return static_cast<double>(eventCount<process::HttpEvent>());
  }

  double _tasks_staging();
  double _tasks_starting();
  double _tasks_running();

  double _resources_total(const std::string& name);
  double _resources_used(const std::string& name);
  double _resources_percent(const std::string& name);

  process::Time startTime; // Start time used to calculate uptime.

  Option<process::Time> electedTime; // Time when this master is elected.

  // Validates the framework including authorization.
  // Returns None if the framework is valid.
  // Returns Error if the framework is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error> > validate(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  struct BoundedRateLimiter
  {
    BoundedRateLimiter(double qps, Option<uint64_t> _capacity)
      : limiter(new process::RateLimiter(qps)),
        capacity(_capacity),
        messages(0) {}

    process::Owned<process::RateLimiter> limiter;
    const Option<uint64_t> capacity;

    // Number of outstanding messages for this RateLimiter.
    // NOTE: ExitedEvents are throttled but not counted towards
    // the capacity here.
    uint64_t messages;
  };

  // BoundedRateLimiters keyed by the framework principal.
  // Like Metrics::Frameworks, all frameworks of the same principal
  // are throttled together at a common rate limit.
  hashmap<std::string, Option<process::Owned<BoundedRateLimiter> > > limiters;

  // The default limiter is for frameworks not specified in
  // 'flags.rate_limits'.
  Option<process::Owned<BoundedRateLimiter> > defaultLimiter;
};


struct Slave
{
  Slave(const SlaveInfo& _info,
        const SlaveID& _id,
        const process::UPID& _pid,
        const process::Time& time)
    : id(_id),
      info(_info),
      pid(_pid),
      registeredTime(time),
      disconnected(false),
      observer(NULL) {}

  ~Slave() {}

  Task* getTask(const FrameworkID& frameworkId, const TaskID& taskId)
  {
    if (tasks.contains(frameworkId) && tasks[frameworkId].contains(taskId)) {
      return tasks[frameworkId][taskId];
    }
    return NULL;
  }

  void addTask(Task* task)
  {
    CHECK(!tasks[task->framework_id()].contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->framework_id()][task->task_id()] = task;
    LOG(INFO) << "Adding task " << task->task_id()
              << " with resources " << task->resources()
              << " on slave " << id << " (" << info.hostname() << ")";
    resourcesInUse += task->resources();
  }

  void removeTask(Task* task)
  {
    CHECK(tasks[task->framework_id()].contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->framework_id()].erase(task->task_id());
    if (tasks[task->framework_id()].empty()) {
      tasks.erase(task->framework_id());
    }

    killedTasks.remove(task->framework_id(), task->task_id());
    LOG(INFO) << "Removing task " << task->task_id()
              << " with resources " << task->resources()
              << " on slave " << id << " (" << info.hostname() << ")";
    resourcesInUse -= task->resources();
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
    offers.insert(offer);
    VLOG(1) << "Adding offer " << offer->id()
            << " with resources " << offer->resources()
            << " on slave " << id << " (" << info.hostname() << ")";
    resourcesOffered += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.contains(offer)) << "Unknown offer " << offer->id();
    offers.erase(offer);
    VLOG(1) << "Removing offer " << offer->id()
            << " with resources " << offer->resources()
            << " on slave " << id << " (" << info.hostname() << ")";
    resourcesOffered -= offer->resources();
  }

  bool hasExecutor(const FrameworkID& frameworkId,
                   const ExecutorID& executorId) const
  {
    return executors.contains(frameworkId) &&
      executors.get(frameworkId).get().contains(executorId);
  }

  void addExecutor(const FrameworkID& frameworkId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(frameworkId, executorInfo.executor_id()))
      << "Duplicate executor " << executorInfo.executor_id()
      << " of framework " << frameworkId;

    executors[frameworkId][executorInfo.executor_id()] = executorInfo;

    // Update the resources in use to reflect running this executor.
    resourcesInUse += executorInfo.resources();
  }

  void removeExecutor(const FrameworkID& frameworkId,
                      const ExecutorID& executorId)
  {
    if (hasExecutor(frameworkId, executorId)) {
      // Update the resources in use to reflect removing this executor.
      resourcesInUse -= executors[frameworkId][executorId].resources();

      executors[frameworkId].erase(executorId);
      if (executors[frameworkId].size() == 0) {
        executors.erase(frameworkId);
      }
    }
  }

  const SlaveID id;
  const SlaveInfo info;

  process::UPID pid;

  process::Time registeredTime;
  Option<process::Time> reregisteredTime;

  // We mark a slave 'disconnected' when it has checkpointing
  // enabled because we expect it reregister after recovery.
  bool disconnected;

  Resources resourcesOffered; // Resources offered.
  Resources resourcesInUse;   // Resources used by tasks and executors.

  // Executors running on this slave.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo> > executors;

  // Tasks present on this slave.
  // TODO(bmahler): The task pointer ownership complexity arises from the fact
  // that we own the pointer here, but it's shared with the Framework struct.
  // We should find a way to eliminate this.
  hashmap<FrameworkID, hashmap<TaskID, Task*> > tasks;

  // Tasks that were asked to kill by frameworks.
  // This is used for reconciliation when the slave re-registers.
  multihashmap<FrameworkID, TaskID> killedTasks;

  // Active offers on this slave.
  hashset<Offer*> offers;

  SlaveObserver* observer;

private:
  Slave(const Slave&);              // No copying.
  Slave& operator = (const Slave&); // No assigning.
};


inline std::ostream& operator << (std::ostream& stream, const Slave& slave)
{
  return stream << slave.id << " at " << slave.pid
                << " (" << slave.info.hostname() << ")";
}


// Information about a connected or completed framework.
struct Framework
{
  Framework(const FrameworkInfo& _info,
            const FrameworkID& _id,
            const process::UPID& _pid,
            const process::Time& time = process::Clock::now())
    : id(_id),
      info(_info),
      pid(_pid),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(MAX_COMPLETED_TASKS_PER_FRAMEWORK) {}

  ~Framework() {}

  Task* getTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    } else {
      return NULL;
    }
  }

  void addTask(Task* task)
  {
    CHECK(!tasks.contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->task_id()] = task;
    resources += task->resources();
  }

  void addCompletedTask(const Task& task)
  {
    // TODO(adam-mesos): Check if completed task already exists.
    completedTasks.push_back(memory::shared_ptr<Task>(new Task(task)));
  }

  void removeTask(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    addCompletedTask(*task);

    tasks.erase(task->task_id());
    resources -= task->resources();
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
    offers.insert(offer);
    resources += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.find(offer) != offers.end())
      << "Unknown offer " << offer->id();

    offers.erase(offer);
    resources -= offer->resources();
  }

  bool hasExecutor(const SlaveID& slaveId,
                   const ExecutorID& executorId)
  {
    return executors.contains(slaveId) &&
      executors[slaveId].contains(executorId);
  }

  void addExecutor(const SlaveID& slaveId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(slaveId, executorInfo.executor_id()))
      << "Duplicate executor " << executorInfo.executor_id()
      << " on slave " << slaveId;

    executors[slaveId][executorInfo.executor_id()] = executorInfo;

    // Update our resources to reflect running this executor.
    resources += executorInfo.resources();
  }

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId)
  {
    if (hasExecutor(slaveId, executorId)) {
      // Update our resources to reflect removing this executor.
      resources -= executors[slaveId][executorId].resources();

      executors[slaveId].erase(executorId);
      if (executors[slaveId].size() == 0) {
        executors.erase(slaveId);
      }
    }
  }

  const FrameworkID id; // TODO(benh): Store this in 'info'.

  const FrameworkInfo info;

  process::UPID pid;

  bool active; // Turns false when framework is being removed.
  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // Tasks that have not yet been launched because they are being
  // validated (e.g., authorized).
  hashmap<TaskID, TaskInfo> pendingTasks;

  hashmap<TaskID, Task*> tasks;

  // NOTE: We use a shared pointer for Task because clang doesn't like
  // Boost's implementation of circular_buffer with Task (Boost
  // attempts to do some memset's which are unsafe).
  boost::circular_buffer<memory::shared_ptr<Task> > completedTasks;

  hashset<Offer*> offers; // Active offers for framework.

  Resources resources; // Total resources (tasks + offers + executors).

  hashmap<SlaveID, hashmap<ExecutorID, ExecutorInfo> > executors;

private:
  Framework(const Framework&);              // No copying.
  Framework& operator = (const Framework&); // No assigning.
};


// Information about an active role.
struct Role
{
  explicit Role(const RoleInfo& _info)
    : info(_info) {}

  void addFramework(Framework* framework)
  {
    frameworks[framework->id] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id);
  }

  Resources resources() const
  {
    Resources resources;
    foreachvalue (Framework* framework, frameworks) {
      resources += framework->resources;
    }

    return resources;
  }

  RoleInfo info;

  hashmap<FrameworkID, Framework*> frameworks;
};


// Implementation of slave admission Registrar operation.
class AdmitSlave : public Operation
{
public:
  explicit AdmitSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    // Check and see if this slave already exists.
    if (slaveIDs->contains(info.id())) {
      if (strict) {
        return Error("Slave already admitted");
      } else {
        return false; // No mutation.
      }
    }

    Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
    slave->mutable_info()->CopyFrom(info);
    slaveIDs->insert(info.id());
    return true; // Mutation.
  }

private:
  const SlaveInfo info;
};


// Implementation of slave readmission Registrar operation.
class ReadmitSlave : public Operation
{
public:
  explicit ReadmitSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    if (slaveIDs->contains(info.id())) {
      return false; // No mutation.
    }

    if (strict) {
      return Error("Slave not yet admitted");
    } else {
      Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
      slave->mutable_info()->CopyFrom(info);
      slaveIDs->insert(info.id());
      return true; // Mutation.
    }
  }

private:
  const SlaveInfo info;
};


// Implementation of slave removal Registrar operation.
class RemoveSlave : public Operation
{
public:
  explicit RemoveSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      const Registry::Slave& slave = registry->slaves().slaves(i);
      if (slave.info().id() == info.id()) {
        registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
        slaveIDs->erase(info.id());
        return true; // Mutation.
      }
    }

    if (strict) {
      return Error("Slave not yet admitted");
    } else {
      return false; // No mutation.
    }
  }

private:
  const SlaveInfo info;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
