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

#include <list>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/resources.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

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

namespace sasl {

class Authenticator; // Forward declaration.

}

namespace master {

using namespace process; // Included to make code easier to read.

// Forward declarations.
namespace allocator {

class Allocator;

}

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
      const std::vector<Task>& tasks);
  void unregisterSlave(
      const SlaveID& slaveId);
  void statusUpdate(
      const StatusUpdate& update,
      const UPID& pid);
  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);
  void deactivateSlave(
      const SlaveID& slaveId);

  // TODO(bmahler): It would be preferred to use a unique libprocess
  // Process identifier (PID is not sufficient) for identifying the
  // framework instance, rather than relying on re-registration time.
  void frameworkFailoverTimeout(
      const FrameworkID& frameworkId,
      const Time& reregisteredTime);

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
  void detected(const Future<Option<MasterInfo> >& pid);

  // Invoked when the contender has lost the candidacy.
  // Made public for testing purposes.
  void lostCandidacy(const Future<Nothing>& lost);

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const UPID& pid);

  void deactivate(Framework* framework);

  // 'promise' is used to signal finish of authentication.
  // 'future' is the future returned by the authenticator.
  void _authenticate(
      const UPID& pid,
      const Owned<Promise<Nothing> >& promise,
      const Future<bool>& future);

  void authenticationTimeout(Future<bool> future);

  void fileAttached(const Future<Nothing>& result, const std::string& path);

  // Return connected frameworks that are not in the process of being removed
  std::vector<Framework*> getActiveFrameworks() const;

  // Invoked when the contender has entered the contest.
  void contended(const Future<Future<Nothing> >& candidacy);

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
  void failoverFramework(Framework* framework, const UPID& newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., kill all of its tasks,
  // remove its offers and reallocate its resources.
  void removeFramework(Slave* slave, Framework* framework);

  // Add a slave.
  void addSlave(Slave* slave, bool reregister = false);

  void readdSlave(Slave* slave,
		  const std::vector<ExecutorInfo>& executorInfos,
		  const std::vector<Task>& tasks);

  // Lose all of a slave's tasks and delete the slave object
  void removeSlave(Slave* slave);

  // Launch a task from a task description, and returned the consumed
  // resources for the task and possibly it's executor.
  Resources launchTask(const TaskInfo& task,
                       Framework* framework,
                       Slave* slave);

  // Remove a task.
  void removeTask(Task* task);

  // Forwards the update to the framework.
  Try<Nothing> forward(const StatusUpdate& update, const UPID& pid);

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
    Http(const Master& _master) : master(_master) {}

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request);

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request);

    // /master/stats.json
    process::Future<process::http::Response> stats(
        const process::http::Request& request);

    // /master/state.json
    process::Future<process::http::Response> state(
        const process::http::Request& request);

    // /master/roles.json
    process::Future<process::http::Response> roles(
        const process::http::Request& request);

    // /master/tasks.json
    process::Future<process::http::Response> tasks(
        const process::http::Request& request);

    const static std::string HEALTH_HELP;
    const static std::string REDIRECT_HELP;
    const static std::string TASKS_HELP;

  private:
    const Master& master;
  } http;

  Master(const Master&);              // No copying.
  Master& operator = (const Master&); // No assigning.

  friend struct SlaveRegistrar;
  friend struct SlaveReregistrar;
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
  Files* files;

  MasterContender* contender;
  MasterDetector* detector;

  MasterInfo info_;

  hashmap<FrameworkID, Framework*> frameworks;

  hashmap<SlaveID, Slave*> slaves;

  // Ideally we could use SlaveIDs to track deactivated slaves.
  // However, we would not know when to remove the SlaveID from this
  // set. After deactivation, the same slave machine can register with
  // the same. Using PIDs allows us to remove the deactivated
  // slave PID once any slave registers with the same PID!
  hashset<UPID> deactivatedSlaves;

  hashmap<OfferID, Offer*> offers;

  hashmap<std::string, Role*> roles;

  // Frameworks that are currently in the process of authentication.
  // 'authenticating' future for a framework is ready when it is
  // authenticated.
  hashmap<UPID, Future<Nothing> > authenticating;

  hashmap<UPID, Owned<sasl::Authenticator> > authenticators;

  // Authenticated frameworks keyed by framework's PID.
  hashset<UPID> authenticated;

  boost::circular_buffer<memory::shared_ptr<Framework> > completedFrameworks;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  // Statistics (initialized in Master::initialize).
  struct {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  Time startTime; // Start time used to calculate uptime.
};


// A connected slave.
struct Slave
{
  Slave(const SlaveInfo& _info,
        const SlaveID& _id,
        const UPID& _pid,
        const Time& time)
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

  UPID pid;

  Time registeredTime;
  Option<Time> reregisteredTime;

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


// Information about a connected or completed framework.
struct Framework
{
  Framework(const FrameworkInfo& _info,
            const FrameworkID& _id,
            const UPID& _pid,
            const Time& time)
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

  UPID pid;

  bool active; // Turns false when framework is being removed.
  Time registeredTime;
  Time reregisteredTime;
  Time unregisteredTime;

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
  Role(const RoleInfo& _info)
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

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
