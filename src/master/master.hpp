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


namespace mesos {
namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

namespace sasl {
class Authenticator;
}

namespace master {

// Forward declarations.
namespace allocator {
class Allocator;
}

class Repairer;
class SlaveObserver;
class WhitelistWatcher;

struct Framework;
struct Slave;
struct Role;
struct OfferVisitor;


class Master : public ProtobufProcess<Master>
{
public:
  Master(allocator::Allocator* allocator,
         Registrar* registrar,
         Repairer* repairer,
         Files* files,
         MasterContender* contender,
         MasterDetector* detector,
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
      const OfferID& offerId,
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

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const process::UPID& pid);
  virtual void visit(const process::MessageEvent& event);

  // Recovers state from the registrar.
  process::Future<Nothing> recover();
  void recoveredSlavesTimeout(const Registry& registry);

  void _registerSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const process::Future<bool>& admit);

  void _reregisterSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const process::Future<bool>& readmit);

  void __reregisterSlave(
      Slave* slave,
      const std::vector<Task>& tasks);

  // 'promise' is used to signal finish of authentication.
  // 'future' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Owned<process::Promise<Nothing> >& promise,
      const process::Future<bool>& future);

  void authenticationTimeout(process::Future<bool> future);

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

  // Add a framework.
  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., kill all of its tasks,
  // remove its offers and reallocate its resources.
  void removeFramework(Slave* slave, Framework* framework);

  // TODO(adam-mesos): Rename deactivate to disconnect, or v.v.
  void deactivate(Framework* framework);
  void disconnect(Slave* slave);
  void removeFrameworksAndOffers(Slave* slave);

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

  // Launch a task from a task description, and returned the consumed
  // resources for the task and possibly it's executor.
  Resources launchTask(const TaskInfo& task,
                       Framework* framework,
                       Slave* slave);

  // Remove a task.
  void removeTask(Task* task);

  // Forwards the update to the framework.
  Try<Nothing> forward(const StatusUpdate& update, const process::UPID& pid);

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  Framework* getFramework(const FrameworkID& frameworkId);
  Slave* getSlave(const SlaveID& slaveId);
  Offer* getOffer(const OfferID& offerId);

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

private:
  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(const Master& _master) : master(_master) {}

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
    const static std::string TASKS_HELP;

  private:
    const Master& master;
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

  MasterInfo info_;

  // Indicates when recovery is complete. Recovery begins once the
  // master is elected as a leader.
  Option<process::Future<Nothing> > recovered;

  struct Slaves
  {
    Slaves() : deactivated(MAX_DEACTIVATED_SLAVES) {}

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

    hashmap<SlaveID, Slave*> activated;

    // Slaves that are in the process of being removed from the
    // registrar. Think of these as being partially removed: we must
    // not answer questions related to these until they are removed
    // from the registry.
    hashset<SlaveID> removing;

    // We track deactivated slaves to preserve the consistency
    // semantics of the pre-registrar code when a non-strict registrar
    // is being used. That is, if we deactivate a slave, we must make
    // an effort to prevent it from (re-)registering, sending updates,
    // etc. We keep a cache here to prevent this from growing in an
    // unbounded manner.
    // TODO(bmahler): Ideally we could use a cache with set semantics.
    Cache<SlaveID, Nothing> deactivated;
  } slaves;

  struct Frameworks
  {
    Frameworks() : completed(MAX_COMPLETED_FRAMEWORKS) {}

    hashmap<FrameworkID, Framework*> activated;
    boost::circular_buffer<memory::shared_ptr<Framework> > completed;
  } frameworks;

  hashmap<OfferID, Offer*> offers;

  hashmap<std::string, Role*> roles;

  // Frameworks/slaves that are currently in the process of authentication.
  // 'authenticating' future for an authenticatee is ready when it is
  // authenticated.
  hashmap<process::UPID, process::Future<Nothing> > authenticating;

  hashmap<process::UPID, process::Owned<sasl::Authenticator> > authenticators;

  // Authenticated frameworks/slaves keyed by PID.
  hashset<process::UPID> authenticated;

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
    Metrics()
      : dropped_messages(
            "master/dropped_messages"),
        framework_registration_messages(
            "master/framework_registration_messages"),
        framework_reregistration_messages(
            "master/framework_reregistration_messages"),
        slave_registration_messages(
            "master/slave_registration_messages"),
        slave_reregistration_messages(
            "master/slave_reregistration_messages"),
        recovery_slave_removals(
            "master/recovery_slave_removals")
    {
      process::metrics::add(dropped_messages);

      process::metrics::add(framework_registration_messages);
      process::metrics::add(framework_reregistration_messages);

      process::metrics::add(slave_registration_messages);
      process::metrics::add(slave_reregistration_messages);

      process::metrics::add(recovery_slave_removals);
    }

    ~Metrics()
    {
      process::metrics::remove(dropped_messages);

      process::metrics::remove(framework_registration_messages);
      process::metrics::remove(framework_reregistration_messages);

      process::metrics::remove(slave_registration_messages);
      process::metrics::remove(slave_reregistration_messages);

      process::metrics::remove(recovery_slave_removals);
    }

    // Message counters.
    // TODO(bmahler): Add counters for other messages: kill task,
    // status update, etc.
    process::metrics::Counter dropped_messages;

    process::metrics::Counter framework_registration_messages;
    process::metrics::Counter framework_reregistration_messages;

    process::metrics::Counter slave_registration_messages;
    process::metrics::Counter slave_reregistration_messages;

    // Recovery counters.
    process::metrics::Counter recovery_slave_removals;
  } metrics;

  process::Time startTime; // Start time used to calculate uptime.
};


// A connected (or disconnected, checkpointing) slave.
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

  void removeTask(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    completedTasks.push_back(memory::shared_ptr<Task>(new Task(*task)));
    tasks.erase(task->task_id());
    resources -= task->resources();
  }

  void addCompletedTask(const Task& task)
  {
    // TODO(adam-mesos): Check if completed task already exists.
    completedTasks.push_back(memory::shared_ptr<Task>(new Task(task)));
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
