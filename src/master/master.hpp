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
#include <memory>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/master/allocator.hpp>

#include <mesos/module/authenticator.hpp>

#include <process/limiter.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>

#include <stout/cache.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>
#include <stout/recordio.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "files/files.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/constants.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/flags.hpp"
#include "master/metrics.hpp"
#include "master/registrar.hpp"
#include "master/validation.hpp"

#include "messages/messages.hpp"

namespace process {
class RateLimiter; // Forward declaration.
}

namespace mesos {

// Forward declarations.
class Authorizer;

namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

class WhitelistWatcher;

namespace master {

class Repairer;
class SlaveObserver;

struct BoundedRateLimiter;
struct Framework;
struct HttpConnection;
struct Role;


struct Slave
{
  Slave(const SlaveInfo& _info,
        const process::UPID& _pid,
        const Option<std::string> _version,
        const process::Time& _registeredTime,
        const Resources& _checkpointedResources,
        const std::vector<ExecutorInfo> executorInfos =
          std::vector<ExecutorInfo>(),
        const std::vector<Task> tasks =
          std::vector<Task>())
    : id(_info.id()),
      info(_info),
      pid(_pid),
      version(_version),
      registeredTime(_registeredTime),
      connected(true),
      active(true),
      checkpointedResources(_checkpointedResources),
      observer(NULL)
  {
    CHECK(_info.has_id());

    Try<Resources> resources = applyCheckpointedResources(
        info.resources(),
        _checkpointedResources);

    // NOTE: This should be validated during slave recovery.
    CHECK_SOME(resources);
    totalResources = resources.get();

    foreach (const ExecutorInfo& executorInfo, executorInfos) {
      CHECK(executorInfo.has_framework_id());
      addExecutor(executorInfo.framework_id(), executorInfo);
    }

    foreach (const Task& task, tasks) {
      addTask(new Task(task));
    }
  }

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
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(!tasks[frameworkId].contains(taskId))
      << "Duplicate task " << taskId << " of framework " << frameworkId;

    tasks[frameworkId][taskId] = task;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources[frameworkId] += task->resources();
    }

    LOG(INFO) << "Adding task " << taskId
              << " with resources " << task->resources()
              << " on slave " << id << " (" << info.hostname() << ")";
  }

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    usedResources[frameworkId] -= task->resources();
    if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
      usedResources.erase(frameworkId);
    }
  }

  void removeTask(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources[frameworkId] -= task->resources();
      if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
        usedResources.erase(frameworkId);
      }
    }

    tasks[frameworkId].erase(taskId);
    if (tasks[frameworkId].empty()) {
      tasks.erase(frameworkId);
    }

    killedTasks.remove(frameworkId, taskId);
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();

    offers.insert(offer);
    offeredResources += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.contains(offer)) << "Unknown offer " << offer->id();

    offeredResources -= offer->resources();
    offers.erase(offer);
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
    usedResources[frameworkId] += executorInfo.resources();
  }

  void removeExecutor(const FrameworkID& frameworkId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(frameworkId, executorId))
      << "Unknown executor " << executorId << " of framework " << frameworkId;

    usedResources[frameworkId] -=
      executors[frameworkId][executorId].resources();

    // XXX Remove.

    executors[frameworkId].erase(executorId);
    if (executors[frameworkId].empty()) {
      executors.erase(frameworkId);
    }
  }

  void apply(const Offer::Operation& operation)
  {
    Try<Resources> resources = totalResources.apply(operation);
    CHECK_SOME(resources);

    totalResources = resources.get();
    checkpointedResources = totalResources.filter(needCheckpointing);
  }

  const SlaveID id;
  const SlaveInfo info;

  process::UPID pid;

  // The Mesos version of the slave. If set, the slave is >= 0.21.0.
  // TODO(bmahler): Use stout's Version when it can parse labels, etc.
  // TODO(bmahler): Make this required once it is always set.
  const Option<std::string> version;

  process::Time registeredTime;
  Option<process::Time> reregisteredTime;

  // Slave becomes disconnected when the socket closes.
  bool connected;

  // Slave becomes deactivated when it gets disconnected. In the
  // future this might also happen via HTTP endpoint.
  // No offers will be made for a deactivated slave.
  bool active;

  // Executors running on this slave.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // Tasks present on this slave.
  // TODO(bmahler): The task pointer ownership complexity arises from the fact
  // that we own the pointer here, but it's shared with the Framework struct.
  // We should find a way to eliminate this.
  hashmap<FrameworkID, hashmap<TaskID, Task*>> tasks;

  // Tasks that were asked to kill by frameworks.
  // This is used for reconciliation when the slave re-registers.
  multihashmap<FrameworkID, TaskID> killedTasks;

  // Active offers on this slave.
  hashset<Offer*> offers;

  hashmap<FrameworkID, Resources> usedResources;  // Active task / executors.
  Resources offeredResources; // Offers.

  // Resources that should be checkpointed by the slave (e.g.,
  // persistent volumes, dynamic reservations, etc). These are either
  // in use by a task/executor, or are available for use and will be
  // re-offered to the framework.
  Resources checkpointedResources;

  // The current total resources of the slave. Note that this is
  // different from 'info.resources()' because this also considers
  // operations (e.g., CREATE, RESERVE) that have been applied and
  // includes revocable resources as well.
  Resources totalResources;

  SlaveObserver* observer;

private:
  Slave(const Slave&);              // No copying.
  Slave& operator=(const Slave&); // No assigning.
};


inline std::ostream& operator<<(std::ostream& stream, const Slave& slave)
{
  return stream << slave.id << " at " << slave.pid
                << " (" << slave.info.hostname() << ")";
}


class Master : public ProtobufProcess<Master>
{
public:
  Master(mesos::master::allocator::Allocator* allocator,
         Registrar* registrar,
         Repairer* repairer,
         Files* files,
         MasterContender* contender,
         MasterDetector* detector,
         const Option<Authorizer*>& authorizer,
         const Option<std::shared_ptr<process::RateLimiter>>&
           slaveRemovalLimiter,
         const Flags& flags = Flags());

  virtual ~Master();

  // Message handlers.
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

  // TODO(vinod): Remove this once the old driver is removed.
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

  void executorMessage(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void registerSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::vector<Resource>& checkpointedResources,
      const std::string& version);

  void reregisterSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::vector<Resource>& checkpointedResources,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version);

  void unregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId);

  void statusUpdate(
      const StatusUpdate& update,
      const process::UPID& pid);

  void reconcileTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskStatus>& statuses);

  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);

  void updateSlave(
      const SlaveID& slaveId,
      const Resources& oversubscribedResources);

  void shutdownSlave(
      const SlaveID& slaveId,
      const std::string& message);

  void authenticate(
      const process::UPID& from,
      const process::UPID& pid);

  // TODO(bmahler): It would be preferred to use a unique libprocess
  // Process identifier (PID is not sufficient) for identifying the
  // framework instance, rather than relying on re-registration time.
  void frameworkFailoverTimeout(
      const FrameworkID& frameworkId,
      const process::Time& reregisteredTime);

  void offer(
      const FrameworkID& framework,
      const hashmap<SlaveID, Resources>& resources);

  // Invoked when there is a newly elected leading master.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo>>& pid);

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
      const std::vector<Resource>& checkpointedResources,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version,
      const process::Future<bool>& readmit);

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();

  virtual void visit(const process::MessageEvent& event);
  virtual void visit(const process::ExitedEvent& event);

  virtual void exited(const process::UPID& pid);
  void exited(const FrameworkID& frameworkId, const HttpConnection& http);
  void _exited(Framework* framework);

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
      const std::vector<Resource>& checkpointedResources,
      const std::string& version,
      const process::Future<bool>& admit);

  void __reregisterSlave(
      Slave* slave,
      const std::vector<Task>& tasks);

  // 'authenticate' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Future<Option<std::string>>& authenticate);

  void authenticationTimeout(process::Future<Option<std::string>> future);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  // Invoked when the contender has entered the contest.
  void contended(const process::Future<process::Future<Nothing>>& candidacy);

  // Task reconciliation, split from the message handler
  // to allow re-use.
  void _reconcileTasks(
      Framework* framework,
      const std::vector<TaskStatus>& statuses);

  // Handles a known re-registering slave by reconciling the master's
  // view of the slave's tasks and executors.
  void reconcile(
      Slave* slave,
      const std::vector<ExecutorInfo>& executors,
      const std::vector<Task>& tasks);

  // Add a framework.
  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Replace the scheduler for a framework with a new HTTP connection,
  // in the event of a scheduler failover.
  void failoverFramework(Framework* framework, const HttpConnection& http);

  void _failoverFramework(Framework* framework);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., remove its tasks and
  // executors and recover the resources.
  void removeFramework(Slave* slave, Framework* framework);

  void disconnect(Framework* framework);
  void deactivate(Framework* framework);

  void disconnect(Slave* slave);
  void deactivate(Slave* slave);

  // Add a slave.
  void addSlave(
      Slave* slave,
      const std::vector<Archive::Framework>& completedFrameworks =
        std::vector<Archive::Framework>());

  // Remove the slave from the registrar. Called when the slave
  // does not re-register in time after a master failover.
  Nothing removeSlave(const Registry::Slave& slave);

  // Remove the slave from the registrar and from the master's state.
  //
  // TODO(bmahler): 'reason' is optional until MESOS-2317 is resolved.
  void removeSlave(
      Slave* slave,
      const std::string& message,
      Option<process::metrics::Counter> reason = None());

  void _removeSlave(
      const SlaveInfo& slaveInfo,
      const std::vector<StatusUpdate>& updates,
      const process::Future<bool>& removed,
      const std::string& message,
      Option<process::metrics::Counter> reason = None());

  // Validates that the framework is authenticated, if required.
  Option<Error> validateFrameworkAuthentication(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  // Returns whether the framework is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeFramework(
      const FrameworkInfo& frameworkInfo);

  // Returns whether the task is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeTask(
      const TaskInfo& task,
      Framework* framework);

  // Add the task and its executor (if not already running) to the
  // framework and slave. Returns the resources consumed as a result,
  // which includes resources for the task and its executor
  // (if not already running).
  Resources addTask(const TaskInfo& task, Framework* framework, Slave* slave);

  // Transitions the task, and recovers resources if the task becomes
  // terminal.
  void updateTask(Task* task, const StatusUpdate& update);

  // Removes the task.
  void removeTask(Task* task);

  // Remove an executor and recover its resources.
  void removeExecutor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Updates slave's resources by applying the given operation. It
  // also updates the allocator and sends a CheckpointResourcesMessage
  // to the slave with slave's current checkpointed resources.
  void applyOfferOperation(
      Framework* framework,
      Slave* slave,
      const Offer::Operation& operation);

  // Forwards the update to the framework.
  void forward(
      const StatusUpdate& update,
      const process::UPID& acknowledgee,
      Framework* framework);

  // Remove an offer after specified timeout
  void offerTimeout(const OfferID& offerId);

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  Framework* getFramework(const FrameworkID& frameworkId);
  Offer* getOffer(const OfferID& offerId);

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

  Option<Credentials> credentials;

private:
  void drop(
      const process::UPID& from,
      const scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const Offer::Operation& operation,
      const std::string& message);

  // Call handlers.
  void receive(
      const process::UPID& from,
      const scheduler::Call& call);

  void subscribe(
      HttpConnection http,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      HttpConnection http,
      const scheduler::Call::Subscribe& subscribe,
      const process::Future<bool>& authorized);

  void subscribe(
      const process::UPID& from,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      const process::UPID& from,
      const scheduler::Call::Subscribe& subscribe,
      const process::Future<bool>& authorized);

  void teardown(Framework* framework);

  void accept(
      Framework* framework,
      const scheduler::Call::Accept& accept);

  void _accept(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& offeredResources,
    const scheduler::Call::Accept& accept,
    const process::Future<std::list<process::Future<bool>>>& authorizations);

  void decline(
      Framework* framework,
      const scheduler::Call::Decline& decline);

  void revive(Framework* framework);

  void kill(
      Framework* framework,
      const scheduler::Call::Kill& kill);

  void shutdown(
      Framework* framework,
      const scheduler::Call::Shutdown& shutdown);

  void acknowledge(
      Framework* framework,
      const scheduler::Call::Acknowledge& acknowledge);

  void reconcile(
      Framework* framework,
      const scheduler::Call::Reconcile& reconcile);

  void message(
      Framework* framework,
      const scheduler::Call::Message& message);

  void request(
      Framework* framework,
      const scheduler::Call::Request& request);

  bool elected() const
  {
    return leader.isSome() && leader.get() == info_;
  }

  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master) {}

    // Logs the request, route handlers can compose this with the
    // desired request handler to get consistent request logging.
    static void log(const process::http::Request& request);

    // /api/v1/scheduler
    process::Future<process::http::Response> scheduler(
        const process::http::Request& request) const;

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request) const;

    // /master/observe
    process::Future<process::http::Response> observe(
        const process::http::Request& request) const;

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request) const;

    // /master/roles.json
    process::Future<process::http::Response> roles(
        const process::http::Request& request) const;

    // /master/teardown and /master/shutdown (deprecated).
    process::Future<process::http::Response> teardown(
        const process::http::Request& request) const;

    // /master/slaves
    process::Future<process::http::Response> slaves(
        const process::http::Request& request) const;

    // /master/state.json
    process::Future<process::http::Response> state(
        const process::http::Request& request) const;

    // /master/state-summary
    process::Future<process::http::Response> stateSummary(
        const process::http::Request& request) const;

    // /master/tasks.json
    process::Future<process::http::Response> tasks(
        const process::http::Request& request) const;

    const static std::string SCHEDULER_HELP;
    const static std::string HEALTH_HELP;
    const static std::string OBSERVE_HELP;
    const static std::string REDIRECT_HELP;
    const static std::string ROLES_HELP;
    const static std::string TEARDOWN_HELP;
    const static std::string SLAVES_HELP;
    const static std::string STATE_HELP;
    const static std::string STATESUMMARY_HELP;
    const static std::string TASKS_HELP;

  private:
    // Helper for doing authentication, returns the credential used if
    // the authentication was successful (or none if no credentials
    // have been given to the master), otherwise an Error.
    Result<Credential> authenticate(
        const process::http::Request& request) const;

    // Continuations.
    process::Future<process::http::Response> _teardown(
        const FrameworkID& id,
        bool authorized = true) const;

    Master* master;
  };

  Master(const Master&);              // No copying.
  Master& operator=(const Master&); // No assigning.

  friend struct Framework;
  friend struct Metrics;

  // NOTE: Since 'getOffer' and 'slaves' are protected,
  // we need to make the following functions friends.
  friend Offer* validation::offer::getOffer(
      Master* master, const OfferID& offerId);

  friend Slave* validation::offer::getSlave(
      Master* master, const SlaveID& slaveId);

  const Flags flags;

  Option<MasterInfo> leader; // Current leading master.

  mesos::master::allocator::Allocator* allocator;
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
  Option<process::Future<Nothing>> recovered;

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

    // Registered slaves are indexed by SlaveID and UPID. Note that
    // iteration is supported but is exposed as iteration over a
    // hashmap<SlaveID, Slave*> since it is tedious to convert
    // the map's key/value iterator into a value iterator.
    //
    // TODO(bmahler): Consider pulling in boost's multi_index,
    // or creating a simpler indexing abstraction in stout.
    struct
    {
      bool contains(const SlaveID& slaveId) const
      {
        return ids.contains(slaveId);
      }

      bool contains(const process::UPID& pid) const
      {
        return pids.contains(pid);
      }

      Slave* get(const SlaveID& slaveId) const
      {
        return ids.get(slaveId).getOrElse(NULL);
      }

      Slave* get(const process::UPID& pid) const
      {
        return pids.get(pid).getOrElse(NULL);
      }

      void put(Slave* slave)
      {
        CHECK_NOTNULL(slave);
        ids[slave->id] = slave;
        pids[slave->pid] = slave;
      }

      void remove(Slave* slave)
      {
        CHECK_NOTNULL(slave);
        ids.erase(slave->id);
        pids.erase(slave->pid);
      }

      void clear()
      {
        ids.clear();
        pids.clear();
      }

      size_t size() const { return ids.size(); }

      typedef hashmap<SlaveID, Slave*>::iterator iterator;
      typedef hashmap<SlaveID, Slave*>::const_iterator const_iterator;

      iterator begin() { return ids.begin(); }
      iterator end()   { return ids.end();   }

      const_iterator begin() const { return ids.begin(); }
      const_iterator end()   const { return ids.end();   }

    private:
      hashmap<SlaveID, Slave*> ids;
      hashmap<process::UPID, Slave*> pids;
    } registered;

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

    // This rate limiter is used to limit the removal of slaves failing
    // health checks.
    // NOTE: Using a 'shared_ptr' here is OK because 'RateLimiter' is
    // a wrapper around libprocess process which is thread safe.
    Option<std::shared_ptr<process::RateLimiter>> limiter;

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
    Frameworks(const Flags& masterFlags)
      : completed(masterFlags.max_completed_frameworks) {}

    hashmap<FrameworkID, Framework*> registered;
    boost::circular_buffer<std::shared_ptr<Framework>> completed;

    // Principals of frameworks keyed by PID.
    // NOTE: Multiple PIDs can map to the same principal. The
    // principal is None when the framework doesn't specify it.
    // The differences between this map and 'authenticated' are:
    // 1) This map only includes *registered* frameworks. The mapping
    //    is added when a framework (re-)registers.
    // 2) This map includes unauthenticated frameworks (when Master
    //    allows them) if they have principals specified in
    //    FrameworkInfo.
    hashmap<process::UPID, Option<std::string>> principals;

    // BoundedRateLimiters keyed by the framework principal.
    // Like Metrics::Frameworks, all frameworks of the same principal
    // are throttled together at a common rate limit.
    hashmap<std::string, Option<process::Owned<BoundedRateLimiter>>> limiters;

    // The default limiter is for frameworks not specified in
    // 'flags.rate_limits'.
    Option<process::Owned<BoundedRateLimiter>> defaultLimiter;
  } frameworks;

  hashmap<OfferID, Offer*> offers;
  hashmap<OfferID, process::Timer> offerTimers;

  hashmap<std::string, Role*> roles;

  // Authenticator names as supplied via flags.
  std::vector<std::string> authenticatorNames;

  Option<Authenticator*> authenticator;

  // Frameworks/slaves that are currently in the process of authentication.
  // 'authenticating' future is completed when authenticator
  // completes authentication.
  // The future is removed from the map when master completes authentication.
  hashmap<process::UPID, process::Future<Option<std::string>>> authenticating;

  // Principals of authenticated frameworks/slaves keyed by PID.
  hashmap<process::UPID, std::string> authenticated;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  // NOTE: It is safe to use a 'shared_ptr' because 'Metrics' is
  // thread safe.
  // TODO(dhamon): This does not need to be a shared_ptr. Metrics contains
  // copyable metric types only.
  std::shared_ptr<Metrics> metrics;

  // Gauge handlers.
  double _uptime_secs()
  {
    return (process::Clock::now() - startTime).secs();
  }

  double _elected()
  {
    return elected() ? 1 : 0;
  }

  double _slaves_connected();
  double _slaves_disconnected();
  double _slaves_active();
  double _slaves_inactive();

  double _frameworks_connected();
  double _frameworks_disconnected();
  double _frameworks_active();
  double _frameworks_inactive();

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

  double _resources_revocable_total(const std::string& name);
  double _resources_revocable_used(const std::string& name);
  double _resources_revocable_percent(const std::string& name);

  process::Time startTime; // Start time used to calculate uptime.

  Option<process::Time> electedTime; // Time when this master is elected.

  // Validates the framework including authorization.
  // Returns None if the framework is valid.
  // Returns Error if the framework is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error>> validate(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);
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


inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework);


// Represents the streaming HTTP connection to a framework.
struct HttpConnection
{
  HttpConnection(const process::http::Pipe::Writer& _writer,
                 ContentType _contentType)
    : writer(_writer),
      contentType(_contentType),
      encoder(lambda::bind(serialize, contentType, lambda::_1)) {}

  // Converts the message to an Event before sending.
  template <typename Message>
  bool send(const Message& message)
  {
    // We need to evolve the internal 'message' into a
    // 'v1::scheduler::Event'.
    return writer.write(encoder.encode(evolve(message)));
  }

  bool close()
  {
    return writer.close();
  }

  process::Future<Nothing> closed() const
  {
    return writer.readerClosed();
  }

  process::http::Pipe::Writer writer;
  ContentType contentType;
  ::recordio::Encoder<v1::scheduler::Event> encoder;
};


// This process periodically sends heartbeats to a scheduler on the
// given HTTP connection.
class Heartbeater : public process::Process<Heartbeater>
{
public:
  Heartbeater(const FrameworkID& _frameworkId,
              const HttpConnection& _http,
              const Duration& _interval)
    : process::ProcessBase(process::ID::generate("heartbeater")),
      frameworkId(_frameworkId),
      http(_http),
      interval(_interval) {}

protected:
  virtual void initialize() override
  {
    heartbeat();
  }

private:
  void heartbeat()
  {
    // Only send a heartbeat if the connection is not closed.
    if (http.closed().isPending()) {
      VLOG(1) << "Sending heartbeat to " << frameworkId;

      scheduler::Event event;
      event.set_type(scheduler::Event::HEARTBEAT);

      http.send(event);
    }

    process::delay(interval, self(), &Self::heartbeat);
  }

  const FrameworkID frameworkId;
  HttpConnection http;
  const Duration interval;
};


// Information about a connected or completed framework.
// TODO(bmahler): Keeping the task and executor information in sync
// across the Slave and Framework structs is error prone!
struct Framework
{
  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            const process::UPID& _pid,
            const process::Time& time = process::Clock::now())
    : master(_master),
      info(_info),
      pid(_pid),
      connected(true),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(masterFlags.max_completed_tasks_per_framework) {}

  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            const HttpConnection& _http,
            const process::Time& time = process::Clock::now())
    : master(_master),
      info(_info),
      http(_http),
      connected(true),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(masterFlags.max_completed_tasks_per_framework) {}

  ~Framework()
  {
    if (http.isSome()) {
      cleanupConnection();
    }
  }

  Task* getTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    }

    return NULL;
  }

  void addTask(Task* task)
  {
    CHECK(!tasks.contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->task_id()] = task;

    if (!protobuf::isTerminalState(task->state())) {
      totalUsedResources += task->resources();
      usedResources[task->slave_id()] += task->resources();
    }
  }

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    totalUsedResources -= task->resources();
    usedResources[task->slave_id()] -= task->resources();
    if (usedResources[task->slave_id()].empty()) {
      usedResources.erase(task->slave_id());
    }
  }

  // Sends a message to the connected framework.
  template <typename Message>
  void send(const Message& message)
  {
    if (!connected) {
      LOG(WARNING) << "Master attempted to send message to disconnected"
                   << " framework " << *this;
    }

    if (http.isSome()) {
      if (!http.get().send(message)) {
        LOG(WARNING) << "Unable to send event to framework " << *this << ":"
                     << " connection closed";
      }
    } else {
      CHECK_SOME(pid);
      master->send(pid.get(), message);
    }
  }

  void addCompletedTask(const Task& task)
  {
    // TODO(adam-mesos): Check if completed task already exists.
    completedTasks.push_back(std::shared_ptr<Task>(new Task(task)));
  }

  void removeTask(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    if (!protobuf::isTerminalState(task->state())) {
      totalUsedResources -= task->resources();
      usedResources[task->slave_id()] -= task->resources();
      if (usedResources[task->slave_id()].empty()) {
        usedResources.erase(task->slave_id());
      }
    }

    addCompletedTask(*task);

    tasks.erase(task->task_id());
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
    offers.insert(offer);
    totalOfferedResources += offer->resources();
    offeredResources[offer->slave_id()] += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.find(offer) != offers.end())
      << "Unknown offer " << offer->id();

    totalOfferedResources -= offer->resources();
    offeredResources[offer->slave_id()] -= offer->resources();
    if (offeredResources[offer->slave_id()].empty()) {
      offeredResources.erase(offer->slave_id());
    }

    offers.erase(offer);
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
    totalUsedResources += executorInfo.resources();
    usedResources[slaveId] += executorInfo.resources();
  }

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(slaveId, executorId))
      << "Unknown executor " << executorId
      << " of framework " << id()
      << " of slave " << slaveId;

    totalUsedResources -= executors[slaveId][executorId].resources();
    usedResources[slaveId] -= executors[slaveId][executorId].resources();
    if (usedResources[slaveId].empty()) {
      usedResources.erase(slaveId);
    }

    executors[slaveId].erase(executorId);
    if (executors[slaveId].empty()) {
      executors.erase(slaveId);
    }
  }

  const FrameworkID id() const { return info.id(); }

  // Update fields in 'info' using those in 'source'. Currently this
  // only updates 'name', 'failover_timeout', 'hostname', 'webui_url',
  // 'capabilities', and 'labels'.
  void updateFrameworkInfo(const FrameworkInfo& source)
  {
    // TODO(jmlvanre): We can't check 'FrameworkInfo.id' yet because
    // of MESOS-2559. Once this is fixed we can 'CHECK' that we only
    // merge 'info' from the same framework 'id'.

    // TODO(jmlvanre): Merge other fields as per design doc in
    // MESOS-703.

    if (source.user() != info.user()) {
      LOG(WARNING) << "Can not update FrameworkInfo.user to '" << info.user()
                   << "' for framework " << id() << ". Check MESOS-703";
    }

    info.set_name(source.name());

    if (source.has_failover_timeout()) {
      info.set_failover_timeout(source.failover_timeout());
    } else {
      info.clear_failover_timeout();
    }

    if (source.checkpoint() != info.checkpoint()) {
      LOG(WARNING) << "Can not update FrameworkInfo.checkpoint to '"
                   << stringify(info.checkpoint()) << "' for framework " << id()
                   << ". Check MESOS-703";
    }

    if (source.role() != info.role()) {
      LOG(WARNING) << "Can not update FrameworkInfo.role to '" << info.role()
                   << "' for framework " << id() << ". Check MESOS-703";
    }

    if (source.has_hostname()) {
      info.set_hostname(source.hostname());
    } else {
      info.clear_hostname();
    }

    if (source.principal() != info.principal()) {
      LOG(WARNING) << "Can not update FrameworkInfo.principal to '"
                   << info.principal() << "' for framework " << id()
                   << ". Check MESOS-703";
    }

    if (source.has_webui_url()) {
      info.set_webui_url(source.webui_url());
    } else {
      info.clear_webui_url();
    }

    // TODO(aditidixit): Add the case where the capabilities are
    // previously set but now being unset. (MESOS-2880)

    if (source.capabilities_size() > 0) {
      info.mutable_capabilities()->CopyFrom(source.capabilities());
    }

    if (source.has_labels()) {
      info.mutable_labels()->CopyFrom(source.labels());
    } else {
      info.clear_labels();
    }
  }

  void updateConnection(const process::UPID& newPid)
  {
    // Cleanup the HTTP connnection if this is a downgrade from HTTP
    // to PID. Note that the connection may already be closed.
    if (http.isSome()) {
      cleanupConnection();
    }

    // TODO(benh): unlink(oldPid);
    pid = newPid;
  }

  void updateConnection(const HttpConnection& newHttp)
  {
    if (pid.isSome()) {
      // Wipe the PID if this is an upgrade from PID to HTTP.
      // TODO(benh): unlink(oldPid);
      pid = None();
    } else {
      // Cleanup the old HTTP connection.
      // Note that master creates a new HTTP connection for every
      // subscribe request, so 'newHttp' should always be different
      // from 'http'.
      cleanupConnection();
    }

    CHECK_NONE(http);

    http = newHttp;
  }

  // Closes the connection and stops the heartbeat.
  // TODO(vinod): Currently 'connected' variable is set separately
  // from this method. We need to make sure these are in sync.
  void cleanupConnection()
  {
    CHECK_SOME(http);

    if (connected && !http.get().close()) {
      LOG(WARNING) << "Failed to close HTTP pipe for " << *this;
    }

    http = None();

    CHECK_SOME(heartbeater);

    terminate(heartbeater.get().get());
    wait(heartbeater.get().get());

    heartbeater = None();
  }

  void heartbeat()
  {
    CHECK_NONE(heartbeater);
    CHECK_SOME(http);

    // TODO(vinod): Make heartbeat interval configurable and include
    // this information in the SUBSCRIBED response.
    heartbeater =
      new Heartbeater(info.id(), http.get(), DEFAULT_HEARTBEAT_INTERVAL);

    process::spawn(heartbeater.get().get());
  }

  Master* const master;

  FrameworkInfo info;

  // Frameworks can either be connected via HTTP or by message
  // passing (scheduler driver). Exactly one of 'http' and 'pid'
  // will be set according to the last connection made by the
  // framework.
  Option<HttpConnection> http;
  Option<process::UPID> pid;

  // Framework becomes disconnected when the socket closes.
  bool connected;

  // Framework becomes deactivated when it is disconnected or
  // the master receives a DeactivateFrameworkMessage.
  // No offers will be made to a deactivated framework.
  bool active;

  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // Tasks that have not yet been launched because they are currently
  // being authorized.
  hashmap<TaskID, TaskInfo> pendingTasks;

  hashmap<TaskID, Task*> tasks;

  // NOTE: We use a shared pointer for Task because clang doesn't like
  // Boost's implementation of circular_buffer with Task (Boost
  // attempts to do some memset's which are unsafe).
  boost::circular_buffer<std::shared_ptr<Task>> completedTasks;

  hashset<Offer*> offers; // Active offers for framework.

  hashmap<SlaveID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // NOTE: For the used and offered resources below, we keep the
  // total as well as partitioned by SlaveID.
  // We expose the total resources via the HTTP endpoint, and we
  // keep a running total of the resources because looping over the
  // slaves to sum the resources has led to perf issues (MESOS-1862).
  // We keep the resources partitioned by SlaveID because non-scalar
  // resources can be lost when summing them up across multiple
  // slaves (MESOS-2373).
  //
  // Also note that keeping the totals is safe even though it yields
  // incorrect results for non-scalar resources.
  //   (1) For overlapping set items / ranges across slaves, these
  //       will get added N times but only represented once.
  //   (2) When an initial subtraction occurs (N-1), the resource is
  //       no longer represented. (This is the source of the bug).
  //   (3) When any further subtractions occur (N-(1+M)), the
  //       Resources simply ignores the subtraction since there's
  //       nothing to remove, so this is safe for now.

  // TODO(mpark): Strip the non-scalar resources out of the totals
  // in order to avoid reporting incorrect statistics (MESOS-2623).

  // Active task / executor resources.
  Resources totalUsedResources;
  hashmap<SlaveID, Resources> usedResources;

  // Offered resources.
  Resources totalOfferedResources;
  hashmap<SlaveID, Resources> offeredResources;

  // This is only set for HTTP frameworks.
  Option<process::Owned<Heartbeater>> heartbeater;

private:
  Framework(const Framework&);              // No copying.
  Framework& operator=(const Framework&); // No assigning.
};


  inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework)
{
  // TODO(vinod): Also log the hostname once FrameworkInfo is properly
  // updated on framework failover (MESOS-1784).
  stream << framework.id() << " (" << framework.info.name() << ")";

  if (framework.pid.isSome()) {
    stream << " at " << framework.pid.get();
  }

  return stream;
}


// Information about an active role.
struct Role
{
  explicit Role(const mesos::master::RoleInfo& _info)
    : info(_info) {}

  void addFramework(Framework* framework)
  {
    frameworks[framework->id()] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id());
  }

  Resources resources() const
  {
    Resources resources;
    foreachvalue (Framework* framework, frameworks) {
      resources += framework->totalUsedResources;
      resources += framework->totalOfferedResources;
    }

    return resources;
  }

  mesos::master::RoleInfo info;

  hashmap<FrameworkID, Framework*> frameworks;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
