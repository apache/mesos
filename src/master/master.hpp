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

#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <stdint.h>

#include <functional>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/roles.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/master/contender.hpp>
#include <mesos/master/detector.hpp>
#include <mesos/master/master.hpp>

#include <mesos/module/authenticator.hpp>

#include <mesos/quota/quota.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/limiter.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>

#include <stout/boundedhashmap.hpp>
#include <stout/cache.hpp>
#include <stout/circular_buffer.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/multihashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/recordio.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/heartbeater.hpp"
#include "common/http.hpp"
#include "common/resources_utils.hpp"

#include "files/files.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/authorization.hpp"
#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/machine.hpp"
#include "master/metrics.hpp"
#include "master/validation.hpp"

#include "messages/messages.hpp"

namespace process {
class RateLimiter; // Forward declaration.
}

namespace mesos {

// Forward declarations.
class Authorizer;
class ObjectApprovers;

namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

class Registry;
class WhitelistWatcher;

namespace master {

class Master;
class Registrar;
class SlaveObserver;

struct BoundedRateLimiter;
struct Framework;
struct Role;


struct Slave
{
Slave(Master* const _master,
      SlaveInfo _info,
        const process::UPID& _pid,
        const MachineID& _machineId,
        const std::string& _version,
        std::vector<SlaveInfo::Capability> _capabilites,
        const process::Time& _registeredTime,
        std::vector<Resource> _checkpointedResources,
        const Option<UUID>& _resourceVersion,
        std::vector<ExecutorInfo> executorInfos = std::vector<ExecutorInfo>(),
        std::vector<Task> tasks = std::vector<Task>());

  ~Slave();

  Task* getTask(
      const FrameworkID& frameworkId,
      const TaskID& taskId) const;

  void addTask(Task* task);

  // Update slave to recover the resources that were previously
  // being used by `task`.
  //
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void recoverResources(Task* task);

  void removeTask(Task* task);

  void addOperation(Operation* operation);

  void recoverResources(Operation* operation);

  void removeOperation(Operation* operation);

  // Marks a non-speculative operation as an orphan when the originating
  // framework is torn down by the master, or when an agent reregisters
  // with operations from unknown frameworks. If the operation is
  // non-terminal, this has the side effect of modifying the agent's
  // total resources, and should therefore be followed by
  // `allocator->updateSlave()`.
  void markOperationAsOrphan(Operation* operation);

  Operation* getOperation(const UUID& uuid) const;

  void addOffer(Offer* offer);

  void removeOffer(Offer* offer);

  void addInverseOffer(InverseOffer* inverseOffer);

  void removeInverseOffer(InverseOffer* inverseOffer);

  bool hasExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId) const;

  void addExecutor(
      const FrameworkID& frameworkId,
      const ExecutorInfo& executorInfo);

  void removeExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  void apply(const std::vector<ResourceConversion>& conversions);

  Try<Nothing> update(
      const SlaveInfo& info,
      const std::string& _version,
      const std::vector<SlaveInfo::Capability>& _capabilites,
      const Resources& _checkpointedResources,
      const Option<UUID>& resourceVersion);

  Master* const master;
  const SlaveID id;
  SlaveInfo info;

  const MachineID machineId;

  process::UPID pid;

  // TODO(bmahler): Use stout's Version when it can parse labels, etc.
  std::string version;

  // Agent capabilities.
  protobuf::slave::Capabilities capabilities;

  process::Time registeredTime;
  Option<process::Time> reregisteredTime;

  // Slave becomes disconnected when the socket closes.
  bool connected;

  // Slave becomes deactivated when it gets disconnected, or when the
  // agent is deactivated via the DRAIN_AGENT or DEACTIVATE_AGENT calls.
  // No offers will be made for a deactivated slave.
  bool active;

  // Timer for marking slaves unreachable that become disconnected and
  // don't reregister. This timeout is larger than the slave
  // observer's timeout, so typically the slave observer will be the
  // one to mark such slaves unreachable; this timer is a backup for
  // when a slave responds to pings but does not reregister (e.g.,
  // because agent recovery has hung).
  Option<process::Timer> reregistrationTimer;

  // Executors running on this slave.
  //
  // TODO(bmahler): Make this private to enforce that `addExecutor()`
  // and `removeExecutor()` are used, and provide a const view into
  // the executors.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // Tasks present on this slave.
  //
  // TODO(bmahler): Make this private to enforce that `addTask()` and
  // `removeTask()` are used, and provide a const view into the tasks.
  //
  // TODO(bmahler): The task pointer ownership complexity arises from the fact
  // that we own the pointer here, but it's shared with the Framework struct.
  // We should find a way to eliminate this.
  hashmap<FrameworkID, hashmap<TaskID, Task*>> tasks;

  // Tasks that were asked to kill by frameworks.
  // This is used for reconciliation when the slave reregisters.
  multihashmap<FrameworkID, TaskID> killedTasks;

  // Pending operations or terminal operations that have
  // unacknowledged status updates on this agent.
  hashmap<UUID, Operation*> operations;

  // Pending operations whose originating framework is unknown.
  // These operations could be pending, or terminal with unacknowledged
  // status updates.
  //
  // This list can be populated whenever a framework is torn down in the
  // lifetime of the master, or when an agent reregisters with an operation.
  //
  // If the originating framework is completed, the master will
  // acknowledge any status updates instead of the framework.
  // If an orphan does not belong to a completed framework, the master
  // will only acknowledge status updates after a fixed delay.
  hashset<UUID> orphanedOperations;

  // Active offers on this slave.
  hashset<Offer*> offers;

  // Active inverse offers on this slave.
  hashset<InverseOffer*> inverseOffers;

  // Resources for active task / executors / operations.
  // Note that we maintain multiple copies of each shared resource in
  // `usedResources` as they are used by multiple tasks.
  hashmap<FrameworkID, Resources> usedResources;

  Resources offeredResources; // Offers.

  // Resources that should be checkpointed by the slave (e.g.,
  // persistent volumes, dynamic reservations, etc). These are either
  // in use by a task/executor, or are available for use and will be
  // re-offered to the framework.
  // TODO(jieyu): `checkpointedResources` is only for agent default
  // resources. Resources from resource providers are not included in
  // this field. Consider removing this field.
  Resources checkpointedResources;

  // The current total resources of the slave. Note that this is
  // different from 'info.resources()' because this also considers
  // operations (e.g., CREATE, RESERVE) that have been applied and
  // includes revocable resources and resources from resource
  // providers as well.
  Resources totalResources;

  // Used to establish the relationship between the operation and the
  // resources that the operation is operating on. Each resource
  // provider will keep a resource version UUID, and change it when it
  // believes that the resources from this resource provider are out
  // of sync from the master's view.  The master will keep track of
  // the last known resource version UUID for each resource provider,
  // and attach the resource version UUID in each operation it sends
  // out. The resource provider should reject operations that have a
  // different resource version UUID than that it maintains, because
  // this means the operation is operating on resources that might
  // have already been invalidated.
  Option<UUID> resourceVersion;

  SlaveObserver* observer;

  // Time when this agent was last asked to drain. This field
  // is empty if the agent is not currently draining or drained.
  Option<process::Time> estimatedDrainStartTime;

  struct ResourceProvider {
    ResourceProviderInfo info;
    Resources totalResources;

    // Used to establish the relationship between the operation and the
    // resources that the operation is operating on. Each resource
    // provider will keep a resource version UUID, and change it when it
    // believes that the resources from this resource provider are out
    // of sync from the master's view.  The master will keep track of
    // the last known resource version UUID for each resource provider,
    // and attach the resource version UUID in each operation it sends
    // out. The resource provider should reject operations that have a
    // different resource version UUID than that it maintains, because
    // this means the operation is operating on resources that might
    // have already been invalidated.
    UUID resourceVersion;

    // Pending operations or terminal operations that have
    // unacknowledged status updates.
    hashmap<UUID, Operation*> operations;
  };

  hashmap<ResourceProviderID, ResourceProvider> resourceProviders;

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
  Master(mesos::allocator::Allocator* allocator,
         Registrar* registrar,
         Files* files,
         mesos::master::contender::MasterContender* contender,
         mesos::master::detector::MasterDetector* detector,
         const Option<Authorizer*>& authorizer,
         const Option<std::shared_ptr<process::RateLimiter>>&
           slaveRemovalLimiter,
         const Flags& flags = Flags());

  ~Master() override;

  // Compare this master's capabilities with registry's minimum capability.
  // Return the set of capabilities missing from this master.
  static hashset<std::string> missingMinimumCapabilities(
      const MasterInfo& masterInfo, const Registry& registry);

  // Message handlers.
  void submitScheduler(
      const std::string& name);

  void registerFramework(
      const process::UPID& from,
      RegisterFrameworkMessage&& registerFrameworkMessage);

  void reregisterFramework(
      const process::UPID& from,
      ReregisterFrameworkMessage&& reregisterFrameworkMessage);

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
      LaunchTasksMessage&& launchTasksMessage);

  void reviveOffers(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<std::string>& role);

  void killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      StatusUpdateAcknowledgementMessage&& statusUpdateAcknowledgementMessage);

  void schedulerMessage(
      const process::UPID& from,
      FrameworkToExecutorMessage&& frameworkToExecutorMessage);

  void executorMessage(
      const process::UPID& from,
      ExecutorToFrameworkMessage&& executorToFrameworkMessage);

  void registerSlave(
      const process::UPID& from,
      RegisterSlaveMessage&& registerSlaveMessage);

  void reregisterSlave(
      const process::UPID& from,
      ReregisterSlaveMessage&& incomingMessage);

  void unregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId);

  void statusUpdate(
      StatusUpdateMessage&& statusUpdateMessage);

  void reconcileTasks(
      const process::UPID& from,
      ReconcileTasksMessage&& reconcileTasksMessage);

  void updateOperationStatus(
      UpdateOperationStatusMessage&& update);

  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);

  void updateSlave(UpdateSlaveMessage&& message);

  void updateUnavailability(
      const MachineID& machineId,
      const Option<Unavailability>& unavailability);

  // Marks the agent unreachable and returns whether the agent was
  // marked unreachable. Returns false if the agent is already
  // in a transitioning state or has transitioned into another
  // state (this includes already being marked unreachable).
  // The `duringMasterFailover` parameter specifies whether this
  // agent is transitioning from a recovered state (true) or a
  // registered state (false).
  //
  // Discarding currently not supported.
  //
  // Will not return a failure (this will crash the master
  // internally in the case of a registry failure).
  process::Future<bool> markUnreachable(
      const SlaveInfo& slave,
      bool duringMasterFailover,
      const std::string& message);

  void markGone(const SlaveID& slaveId, const TimeInfo& goneTime);

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
      const FrameworkID& frameworkId,
      const hashmap<std::string, hashmap<SlaveID, Resources>>& resources);

  void inverseOffer(
      const FrameworkID& frameworkId,
      const hashmap<SlaveID, UnavailableResources>& resources);

  // Invoked when there is a newly elected leading master.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo>>& _leader);

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
  void initialize() override;
  void finalize() override;

  void consume(process::MessageEvent&& event) override;
  void consume(process::ExitedEvent&& event) override;

  void exited(const process::UPID& pid) override;
  void exited(
      const FrameworkID& frameworkId,
      const StreamingHttpConnection<v1::scheduler::Event>& http);

  void _exited(Framework* framework);

  // Invoked upon noticing a subscriber disconnection.
  void exited(const id::UUID& id);

  void agentReregisterTimeout(const SlaveID& slaveId);
  Nothing _agentReregisterTimeout(const SlaveID& slaveId);

  // Invoked when the message is ready to be executed after
  // being throttled.
  // 'principal' being None indicates it is throttled by
  // 'defaultLimiter'.
  void throttled(
      process::MessageEvent&& event,
      const Option<std::string>& principal);

  // Continuations of consume().
  void _consume(process::MessageEvent&& event);
  void _consume(process::ExitedEvent&& event);

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
      const process::UPID& pid,
      RegisterSlaveMessage&& registerSlaveMessage,
      const Option<process::http::authentication::Principal>& principal,
      const process::Future<bool>& authorized);

  void __registerSlave(
      const process::UPID& pid,
      RegisterSlaveMessage&& registerSlaveMessage,
      const process::Future<bool>& admit);

  void _reregisterSlave(
      const process::UPID& pid,
      ReregisterSlaveMessage&& incomingMessage,
      const Option<process::http::authentication::Principal>& principal,
      const process::Future<bool>& authorized);

  void __reregisterSlave(
      const process::UPID& pid,
      ReregisterSlaveMessage&& incomingMessage,
      const process::Future<bool>& readmit);

  void ___reregisterSlave(
      const process::UPID& pid,
      ReregisterSlaveMessage&& incomingMessage,
      const process::Future<bool>& updated);

  void updateSlaveFrameworks(
      Slave* slave,
      const std::vector<FrameworkInfo>& frameworks);

  // 'future' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Future<Option<std::string>>& future);

  void authenticationTimeout(process::Future<Option<std::string>> future);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  // Invoked when the contender has entered the contest.
  void contended(const process::Future<process::Future<Nothing>>& candidacy);

  // When a slave that was previously registered with this master
  // reregisters, we need to reconcile the master's view of the
  // slave's tasks and executors.  This function also sends the
  // `SlaveReregisteredMessage`.
  void reconcileKnownSlave(
      Slave* slave,
      const std::vector<ExecutorInfo>& executors,
      const std::vector<Task>& tasks);

  // Add a framework.
  void addFramework(
      Framework* framework,
      ::mesos::allocator::FrameworkOptions&& allocatorOptions);

  // Recover a framework from its `FrameworkInfo`. This happens after
  // master failover, when an agent running one of the framework's
  // tasks reregisters or when the framework itself reregisters,
  // whichever happens first. The result of this function is a
  // registered, inactive framework with state `RECOVERED` and empty
  // FrameworkOptions in the allocator.
  void recoverFramework(const FrameworkInfo& info);

  // Transition a framework from `RECOVERED` to `CONNECTED` state and
  // activate it. This happens at most once after master failover, the
  // first time that the framework reregisters with the new master.
  // Exactly one of `newPid` or `http` must be provided.
  void connectAndActivateRecoveredFramework(
      Framework* framework,
      const Option<process::UPID>& pid,
      const Option<StreamingHttpConnection<v1::scheduler::Event>>& http,
      const process::Owned<ObjectApprovers>& objectApprovers);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(
      Framework* framework,
      const process::UPID& newPid,
      const process::Owned<ObjectApprovers>& objectApprovers);

  // Replace the scheduler for a framework with a new HTTP connection,
  // in the event of a scheduler failover.
  void failoverFramework(
      Framework* framework,
      const StreamingHttpConnection<v1::scheduler::Event>& http,
      const process::Owned<ObjectApprovers>& objectApprovers);

  void _failoverFramework(Framework* framework);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., remove its tasks and
  // executors and recover the resources.
  void removeFramework(Slave* slave, Framework* framework);

  // Performs actions common for all the framework update paths.
  //
  // NOTE: the fields 'id', 'principal', 'name' and 'checkpoint' in the
  // 'frameworkInfo' should have the same values as in 'framework->info',
  // otherwise this method terminates the program.
  //
  // TODO(asekretenko): Make sending FrameworkInfo updates to slaves, API
  // subscribers and anywhere else a responsibility of this method -
  // currently is is not, see MESOS-9746. After that we can remove the
  // 'sendFrameworkUpdates()' method.
  void updateFramework(
      Framework* framework,
      const FrameworkInfo& frameworkInfo,
      ::mesos::scheduler::OfferConstraints&& offerConstraints,
      ::mesos::allocator::FrameworkOptions&& allocatorOptions);

  void sendFrameworkUpdates(const Framework& framework);

  void disconnect(Framework* framework);
  void deactivate(Framework* framework, bool rescind);

  void disconnect(Slave* slave);

  // Removes the agent from the resource offer cycle (and rescinds active
  // offers). Other aspects of the agent will continue to function normally.
  void deactivate(Slave* slave);

  // Add a slave.
  void addSlave(
      Slave* slave,
      std::vector<Archive::Framework>&& completedFrameworks);

  void _markUnreachable(
      const SlaveInfo& slave,
      const TimeInfo& unreachableTime,
      bool duringMasterFailover,
      const std::string& message,
      bool registrarResult);

  void sendSlaveLost(const SlaveInfo& slaveInfo);

  // Remove the slave from the registrar and from the master's state.
  //
  // TODO(bmahler): 'reason' is optional until MESOS-2317 is resolved.
  void removeSlave(
      Slave* slave,
      const std::string& message,
      Option<process::metrics::Counter> reason = None());

  // Removes an agent from the master's state in the following cases:
  // * When maintenance is started on an agent
  // * When an agent registers with a new ID from a previously-known IP + port
  // * When an agent unregisters itself with an `UnregisterSlaveMessage`
  void _removeSlave(
      Slave* slave,
      const process::Future<bool>& registrarResult,
      const std::string& removalCause,
      Option<process::metrics::Counter> reason = None());

  // Removes an agent from the master's state in the following cases:
  // * When marking an agent unreachable
  // * When marking an agent gone
  //
  // NOTE that in spite of the name `__removeSlave()`, this function is NOT a
  // continuation of `_removeSlave()`. Rather, these two functions perform
  // similar logic for slightly different cases.
  //
  // TODO(greggomann): refactor `_removeSlave` and `__removeSlave` into a single
  // common helper function. (See MESOS-9550)
  void __removeSlave(
      Slave* slave,
      const std::string& message,
      const Option<TimeInfo>& unreachableTime);

  // Validates that the framework is authenticated, if required.
  Option<Error> validateFrameworkAuthentication(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  // Returns whether the principal is authorized for the specified
  // action-object pair.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorize(
      const Option<process::http::authentication::Principal>& principal,
      authorization::ActionObject&& actionObject);

  // Overload of authorize() for cases which require multiple action-object
  // pairs to be authorized simultaneously.
  process::Future<bool> authorize(
      const Option<process::http::authentication::Principal>& principal,
      std::vector<authorization::ActionObject>&& actionObjects);

  // Determine if a new executor needs to be launched.
  bool isLaunchExecutor (
      const ExecutorID& executorId,
      Framework* framework,
      Slave* slave) const;

  // Add executor to the framework and slave.
  void addExecutor(
      const ExecutorInfo& executorInfo,
      Framework* framework,
      Slave* slave);

  // Add task to the framework and slave.
  void addTask(const TaskInfo& task, Framework* framework, Slave* slave);

  // Transitions the task, and recovers resources if the task becomes
  // terminal.
  void updateTask(Task* task, const StatusUpdate& update);

  // Removes the task. `unreachable` indicates whether the task is removed due
  // to being unreachable. Note that we cannot rely on the task state because
  // it may not reflect unreachability due to being set to TASK_LOST for
  // backwards compatibility.
  void removeTask(Task* task, bool unreachable = false);

  // Remove an executor and recover its resources.
  void removeExecutor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Adds the given operation to the framework and the agent.
  void addOperation(
      Framework* framework,
      Slave* slave,
      Operation* operation);

  // Transitions the operation, and updates and recovers resources if
  // the operation becomes terminal. If `convertResources` is `false`
  // only the consumed resources of terminal operations are recovered,
  // but no resources are converted.
  void updateOperation(
      Operation* operation,
      const UpdateOperationStatusMessage& update,
      bool convertResources = true);

  // Remove the operation.
  void removeOperation(Operation* operation);

  // Send operation update for all operations on the agent.
  void sendBulkOperationFeedback(
    Slave* slave,
    OperationState operationState,
    const std::string& message);

  // Attempts to update the allocator by applying the given operation.
  // If successful, updates the slave's resources, sends a
  // 'CheckpointResourcesMessage' to the slave with the updated
  // checkpointed resources, and returns a 'Future' with 'Nothing'.
  // Otherwise, no action is taken and returns a failed 'Future'.
  process::Future<Nothing> apply(
      Slave* slave,
      const Offer::Operation& operation);

  // Forwards the update to the framework.
  void forward(
      const StatusUpdate& update,
      const process::UPID& acknowledgee,
      Framework* framework);

  // Remove an offer after specified timeout
  void offerTimeout(const OfferID& offerId);

  // Methods for removing an offer and handling associated resources.
  // Both recover the resources in the allocator (optionally setting offer
  // filters) and remove the offer in the master. `rescindOffer` further
  // notifies the framework about the rescind.
  //
  // NOTE: the `filters` field in `rescindOffers` is needed only as
  // a workaround for the race between the master and the allocator
  // which happens when the master tries to free up resources to satisfy
  // operator initiated operations.
  void rescindOffer(Offer* offer, const Option<Filters>& filters = None());
  void discardOffer(Offer* offer, const Option<Filters>& filters = None());

  // Helper for rescindOffer() /  discardOffer() / _accept().
  // Do not use directly.
  //
  // The offer must belong to the framework.
  void _removeOffer(Framework* framework, Offer* offer);

  // Remove an inverse offer after specified timeout
  void inverseOfferTimeout(const OfferID& inverseOfferId);

  // Remove an inverse offer and optionally rescind it as well.
  void removeInverseOffer(InverseOffer* inverseOffer, bool rescind = false);

  bool isCompletedFramework(const FrameworkID& frameworkId) const;

  Framework* getFramework(const FrameworkID& frameworkId) const;
  Offer* getOffer(const OfferID& offerId) const;
  InverseOffer* getInverseOffer(const OfferID& inverseOfferId) const;

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

private:
  // Updates the agent's resources by applying the given operation.
  // Sends either `ApplyOperationMessage` or
  // `CheckpointResourcesMessage` (with updated checkpointed
  // resources) to the agent depending on if the agent has
  // `RESOURCE_PROVIDER` capability.
  void _apply(
      Slave* slave,
      Framework* framework,
      const Offer::Operation& operationInfo);

  void drop(
      const process::UPID& from,
      const mesos::scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const Offer::Operation& operation,
      const std::string& message);

  void drop(
      Framework* framework,
      const mesos::scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const mesos::scheduler::Call::Suppress& suppress,
      const std::string& message);

  void drop(
      Framework* framework,
      const mesos::scheduler::Call::Revive& revive,
      const std::string& message);

  // Call handlers.
  void receive(
      const process::UPID& from,
      mesos::scheduler::Call&& call);

  void subscribe(
      StreamingHttpConnection<v1::scheduler::Event> http,
      mesos::scheduler::Call::Subscribe&& subscribe);

  void _subscribe(
      StreamingHttpConnection<v1::scheduler::Event> http,
      FrameworkInfo&& frameworkInfo,
      scheduler::OfferConstraints&& offerConstraints,
      bool force,
      ::mesos::allocator::FrameworkOptions&& allocatorOptions,
      const process::Future<process::Owned<ObjectApprovers>>& objectApprovers);

  void subscribe(
      const process::UPID& from,
      mesos::scheduler::Call::Subscribe&& subscribe);

  void _subscribe(
      const process::UPID& from,
      FrameworkInfo&& frameworkInfo,
      scheduler::OfferConstraints&& offerConstraints,
      bool force,
      ::mesos::allocator::FrameworkOptions&& allocatorOptions,
      const process::Future<process::Owned<ObjectApprovers>>& objectApprovers);

  // Update framework via SchedulerDriver (i.e. no response
  // code feedback, FrameworkErrorMessage on error).
  void updateFramework(
      const process::UPID& from,
      mesos::scheduler::Call::UpdateFramework&& call);

  // Update framework via HTTP API (i.e. returns 200 OK).
  process::Future<process::http::Response> updateFramework(
      mesos::scheduler::Call::UpdateFramework&& call);

  // Subscribes a client to the 'api/vX' endpoint.
  void subscribe(
      const StreamingHttpConnection<v1::master::Event>& http,
      const process::Owned<ObjectApprovers>& approvers);

  void teardown(Framework* framework);

  void accept(
      Framework* framework,
      mesos::scheduler::Call::Accept&& accept);

  void _accept(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      mesos::scheduler::Call::Accept&& accept);

  void acceptInverseOffers(
      Framework* framework,
      const mesos::scheduler::Call::AcceptInverseOffers& accept);

  void decline(
      Framework* framework,
      mesos::scheduler::Call::Decline&& decline);

  void declineInverseOffers(
      Framework* framework,
      const mesos::scheduler::Call::DeclineInverseOffers& decline);

  // Should be called after each terminal task status update acknowledgement
  // or terminal operation acknowledgement. If an agent is draining, this
  // checks if all pending tasks or operations have terminated and then
  // transitions the DRAINING agent to DRAINED.
  void checkAndTransitionDrainingAgent(Slave* slave);

  void revive(
      Framework* framework,
      const mesos::scheduler::Call::Revive& revive);

  void kill(
      Framework* framework,
      const mesos::scheduler::Call::Kill& kill);

  void shutdown(
      Framework* framework,
      const mesos::scheduler::Call::Shutdown& shutdown);

  void acknowledge(
      Framework* framework,
      mesos::scheduler::Call::Acknowledge&& acknowledge);

  void acknowledgeOperationStatus(
      Framework* framework,
      mesos::scheduler::Call::AcknowledgeOperationStatus&& acknowledge);

  void reconcile(
      Framework* framework,
      mesos::scheduler::Call::Reconcile&& reconcile);

  void reconcileOperations(
      Framework* framework,
      mesos::scheduler::Call::ReconcileOperations&& reconcile);

  void message(
      Framework* framework,
      mesos::scheduler::Call::Message&& message);

  void request(
      Framework* framework,
      const mesos::scheduler::Call::Request& request);

  void suppress(
      Framework* framework,
      const mesos::scheduler::Call::Suppress& suppress);

  bool elected() const
  {
    return leader.isSome() && leader.get() == info_;
  }

  void scheduleRegistryGc();

  void doRegistryGc();

  void _doRegistryGc(
      const hashset<SlaveID>& toRemoveUnreachable,
      const hashset<SlaveID>& toRemoveGone,
      const process::Future<bool>& registrarResult);

  // Returns all roles known to the master, if roles are whitelisted
  // this simply returns the whitelist and any ancestors of roles in
  // the whitelist. Otherwise, this returns:
  //
  //   (1) Roles with configured weight or quota.
  //   (2) Roles with reservations.
  //   (3) Roles with frameworks subscribed or allocated resources.
  //   (4) Ancestor roles of (1), (2), or (3).
  std::vector<std::string> knownRoles() const;

  /**
   * Returns whether the given role is on the whitelist.
   *
   * When using explicit roles, this consults the configured (static)
   * role whitelist. When using implicit roles, any role is allowed
   * (and access control is done via ACLs).
   */
  bool isWhitelistedRole(const std::string& name) const;

  // TODO(bmahler): Store a role tree rather than the existing
  // `roles` map which does not track the tree correctly (it does
  // not insert ancestor entries, nor does it track roles if there
  // are reservations but no frameworks related to them).
  struct RoleResourceBreakdown
  {
  public:
    RoleResourceBreakdown(const Master* const master_, const std::string& role_)
      : master(master_), role(role_) {}

    ResourceQuantities offered() const;
    ResourceQuantities allocated() const;
    ResourceQuantities reserved() const;
    ResourceQuantities consumedQuota() const;

  private:
    const Master* const master;
    const std::string role;
  };

  // Performs stateless and stateful validation of the FrameworkInfo.
  // The stateful validation only uses the master flags and whether
  // the framework is completed.

  // TODO(asekretenko): Pass in the state explicitly to move this function into
  // validation.hpp for unit testability and better separation of concerns.
  Option<Error> validateFramework(const FrameworkInfo& frameworkInfo) const;

  /**
   * Inner class used to namespace the handling of quota requests.
   *
   * It operates inside the Master actor. It is responsible for validating
   * and persisting quota requests, and exposing quota status.
   * @see master/quota_handler.cpp for implementations.
   */
  class QuotaHandler
  {
  public:
    explicit QuotaHandler(Master* _master) : master(_master)
    {
      CHECK_NOTNULL(master);
    }

    // Returns a list of set quotas.
    process::Future<process::http::Response> status(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> status(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> update(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal)
      const;

    process::Future<process::http::Response> set(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> set(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> remove(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> remove(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

  private:
    // Returns an error if the total quota guarantees overcommits
    // the cluster. This is not a quota satisfiability check: it's
    // possible that quota is unsatisfiable even if the quota
    // does not overcommit the cluster.

    // Returns an error if the total quota guarantees overcommits
    // the cluster. This is not a quota satisfiability check: it's
    // possible that quota is unsatisfiable even if the quota
    // does not overcommit the cluster. Specifically, we verify that
    // the following inequality holds:
    //
    //    total cluster capacity >= total quota w/ quota request applied
    //
    // Note, total cluster capacity accounts resources of all the
    // registered agents, including resources from resource providers
    // as well as reservations (both static and dynamic ones).
    static Option<Error> overcommitCheck(
        const std::vector<Resources>& agents,
        const hashmap<std::string, Quota>& quotas,
        const mesos::quota::QuotaInfo& request);

    // We always want to rescind offers after the capacity heuristic. The
    // reason for this is the race between the allocator and the master:
    // it can happen that there are not enough free resources at the
    // allocator's disposal when it is notified about the quota request,
    // but at this point it's too late to rescind.
    //
    // While rescinding, we adhere to the following rules:
    //   * Rescind at least as many resources as there are in the quota request.
    //   * Rescind all offers from an agent in order to make the potential
    //     offer bigger, which increases the chances that a quota'ed framework
    //     will be able to use the offer.
    //   * Rescind offers from at least `numF` agents to make it possible
    //     (but not guaranteed, due to fair sharing) that each framework in
    //     the role for which quota is set gets an offer (`numF` is the
    //     number of frameworks in the quota'ed role). Though this is not
    //     strictly necessary, we think this will increase the debugability
    //     and will improve user experience.
    //
    // TODO(alexr): Consider removing this function once offer management
    // (including rescinding) is moved to allocator.
    void rescindOffers(const mesos::quota::QuotaInfo& request) const;

    process::Future<bool> authorizeGetQuota(
        const Option<process::http::authentication::Principal>& principal,
        const std::string& role) const;

    // This auth function is used for legacy `SET_QUOTA` and `REMOVE_QUOTA`
    // calls. Remove this function after the associated API calls are
    // no longer supported.
    process::Future<bool> authorizeUpdateQuota(
        const Option<process::http::authentication::Principal>& principal,
        const mesos::quota::QuotaInfo& quotaInfo) const;

    process::Future<bool> authorizeUpdateQuotaConfig(
        const Option<process::http::authentication::Principal>& principal,
        const mesos::quota::QuotaConfig& quotaConfig) const;

    process::Future<mesos::quota::QuotaStatus> _status(
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> _update(
        const google::protobuf::RepeatedPtrField<mesos::quota::QuotaConfig>&
          quotaConfigs) const;

    process::Future<process::http::Response> _set(
        const mesos::quota::QuotaRequest& quotaRequest,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> __set(
        const mesos::quota::QuotaInfo& quotaInfo,
        bool forced) const;

    process::Future<process::http::Response> _remove(
        const std::string& role,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> __remove(
        const std::string& role) const;

    // To perform actions related to quota management, we require access to the
    // master data structures. No synchronization primitives are needed here
    // since `QuotaHandler`'s functions are invoked in the Master's actor.
    Master* master;
  };

  /**
   * Inner class used to namespace the handling of /weights requests.
   *
   * It operates inside the Master actor. It is responsible for validating
   * and persisting /weights requests.
   * @see master/weights_handler.cpp for implementations.
   */
  class WeightsHandler
  {
  public:
    explicit WeightsHandler(Master* _master) : master(_master)
    {
      CHECK_NOTNULL(master);
    }

    process::Future<process::http::Response> get(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> get(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> update(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> update(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

  private:
    process::Future<bool> authorizeGetWeight(
        const Option<process::http::authentication::Principal>& principal,
        const WeightInfo& weight) const;

    process::Future<bool> authorizeUpdateWeights(
        const Option<process::http::authentication::Principal>& principal,
        const std::vector<std::string>& roles) const;

    process::Future<std::vector<WeightInfo>> _filterWeights(
        const std::vector<WeightInfo>& weightInfos,
        const std::vector<bool>& roleAuthorizations) const;

    process::Future<std::vector<WeightInfo>> _getWeights(
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response>_updateWeights(
        const Option<process::http::authentication::Principal>& principal,
        const google::protobuf::RepeatedPtrField<WeightInfo>& weightInfos)
            const;

    process::Future<process::http::Response> __updateWeights(
        const std::vector<WeightInfo>& weightInfos) const;

    // Rescind all outstanding offers if any of the 'weightInfos' roles has
    // an active framework.
    void rescindOffers(const std::vector<WeightInfo>& weightInfos) const;

    Master* master;
  };

public:
  // Inner class used to namespace read-only HTTP handlers; these handlers
  // may be executed in parallel.
  //
  // A synchronously executed post-processing step is provided for any
  // cases where the handler is not purely read-only and requires a
  // synchronous write (i.e. it's not feasible to perform the write
  // asynchronously (e.g. SUBSCRIBE cannot have a gap between serving
  // the initial state and registering the subscriber, or else events
  // will be missed in the interim)).
  //
  // The handlers are only permitted to depend on the output content
  // type (derived from the request headers), the request query
  // parameters and the authorization filters to de-duplicate identical
  // responses (this does not de-duplicate all identical responses, e.g.
  // different authz principal but same permissions).
  //
  // NOTE: Most member functions of this class are not routed directly but
  // dispatched from their corresponding handlers in the outer `Http` class.
  // This is because deciding whether an incoming request is read-only often
  // requires some inspection, e.g. distinguishing between "GET" and "POST"
  // requests to the same endpoint.
  class ReadOnlyHandler
  {
  public:
    struct PostProcessing
    {
      struct Subscribe
      {
        process::Owned<ObjectApprovers> approvers;
        StreamingHttpConnection<v1::master::Event> connection;
      };

      // Any additional post-processing cases will add additional
      // cases into this variant.
      Variant<Subscribe> state;
    };

    explicit ReadOnlyHandler(const Master* _master) : master(_master) {}

    // /frameworks
    std::pair<process::http::Response, Option<PostProcessing>> frameworks(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // /roles
    std::pair<process::http::Response, Option<PostProcessing>> roles(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // /slaves
    std::pair<process::http::Response, Option<PostProcessing>> slaves(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // /state
    std::pair<process::http::Response, Option<PostProcessing>> state(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // /state-summary
    std::pair<process::http::Response, Option<PostProcessing>> stateSummary(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // /tasks
    std::pair<process::http::Response, Option<PostProcessing>> tasks(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_STATE
    std::pair<process::http::Response, Option<PostProcessing>> getState(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_AGENTS
    std::pair<process::http::Response, Option<PostProcessing>> getAgents(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_FRAMEWORKS
    std::pair<process::http::Response, Option<PostProcessing>> getFrameworks(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_EXECUTORS
    std::pair<process::http::Response, Option<PostProcessing>> getExecutors(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_OPERATIONS
    std::pair<process::http::Response, Option<PostProcessing>> getOperations(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_TASKS
    std::pair<process::http::Response, Option<PostProcessing>> getTasks(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::GET_ROLES
    std::pair<process::http::Response, Option<PostProcessing>> getRoles(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    // master::Call::SUBSCRIBE
    std::pair<process::http::Response, Option<PostProcessing>> subscribe(
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

  private:
    std::string serializeGetState(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetAgents(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetFrameworks(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetExecutors(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetOperations(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetTasks(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeGetRoles(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::string serializeSubscribe(
        const process::Owned<ObjectApprovers>& approvers) const;

    std::function<void(JSON::ObjectWriter*)> jsonifyGetState(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetAgents(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetFrameworks(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetExecutors(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetOperations(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetTasks(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifyGetRoles(
        const process::Owned<ObjectApprovers>& approvers) const;
    std::function<void(JSON::ObjectWriter*)> jsonifySubscribe(
        const process::Owned<ObjectApprovers>& approvers) const;

    const Master* master;
  };

private:
  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master),
                                     readonlyHandler(_master),
                                     quotaHandler(_master),
                                     weightsHandler(_master) {}

    // /api/v1
    process::Future<process::http::Response> api(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /api/v1/scheduler
    process::Future<process::http::Response> scheduler(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/create-volumes
    process::Future<process::http::Response> createVolumes(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/destroy-volumes
    process::Future<process::http::Response> destroyVolumes(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/flags
    process::Future<process::http::Response> flags(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/frameworks
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> frameworks(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request) const;

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request) const;

    // /master/reserve
    process::Future<process::http::Response> reserve(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/roles
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> roles(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/teardown
    process::Future<process::http::Response> teardown(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/slaves
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> slaves(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/state
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> state(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/state-summary
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> stateSummary(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/tasks
    //
    // NOTE: Requests to this endpoint are batched.
    process::Future<process::http::Response> tasks(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/maintenance/schedule
    process::Future<process::http::Response> maintenanceSchedule(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/maintenance/status
    process::Future<process::http::Response> maintenanceStatus(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/machine/down
    process::Future<process::http::Response> machineDown(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/machine/up
    process::Future<process::http::Response> machineUp(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/unreserve
    process::Future<process::http::Response> unreserve(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/weights
    process::Future<process::http::Response> weights(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/quota (DEPRECATED).
    process::Future<process::http::Response> quota(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    static std::string API_HELP();
    static std::string SCHEDULER_HELP();
    static std::string FLAGS_HELP();
    static std::string FRAMEWORKS_HELP();
    static std::string HEALTH_HELP();
    static std::string REDIRECT_HELP();
    static std::string ROLES_HELP();
    static std::string TEARDOWN_HELP();
    static std::string SLAVES_HELP();
    static std::string STATE_HELP();
    static std::string STATESUMMARY_HELP();
    static std::string TASKS_HELP();
    static std::string MAINTENANCE_SCHEDULE_HELP();
    static std::string MAINTENANCE_STATUS_HELP();
    static std::string MACHINE_DOWN_HELP();
    static std::string MACHINE_UP_HELP();
    static std::string CREATE_VOLUMES_HELP();
    static std::string DESTROY_VOLUMES_HELP();
    static std::string RESERVE_HELP();
    static std::string UNRESERVE_HELP();
    static std::string QUOTA_HELP();
    static std::string WEIGHTS_HELP();

  private:
    JSON::Object __flags() const;

    class FlagsError; // Forward declaration.

    process::Future<Try<JSON::Object, FlagsError>> _flags(
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<std::vector<const Task*>> _tasks(
        const size_t limit,
        const size_t offset,
        const std::string& order,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> _teardown(
        const FrameworkID& id,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> __teardown(
        const FrameworkID& id) const;

    process::Future<process::http::Response> _updateMaintenanceSchedule(
        const mesos::maintenance::Schedule& schedule,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> __updateMaintenanceSchedule(
        const mesos::maintenance::Schedule& schedule,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> ___updateMaintenanceSchedule(
        const mesos::maintenance::Schedule& schedule,
        bool applied) const;

    mesos::maintenance::Schedule _getMaintenanceSchedule(
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<mesos::maintenance::ClusterStatus> _getMaintenanceStatus(
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _startMaintenance(
        const google::protobuf::RepeatedPtrField<MachineID>& machineIds,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _stopMaintenance(
        const google::protobuf::RepeatedPtrField<MachineID>& machineIds,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _drainAgent(
        const SlaveID& slaveId,
        const Option<DurationInfo>& maxGracePeriod,
        const bool markGone,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _deactivateAgent(
        const SlaveID& slaveId,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _reactivateAgent(
        const SlaveID& slaveId,
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> _reserve(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& source,
        const google::protobuf::RepeatedPtrField<Resource>& resources,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> _unreserve(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& resources,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> _createVolumes(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& volumes,
        const Option<process::http::authentication::Principal>&
            principal) const;

    process::Future<process::http::Response> _destroyVolumes(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& volumes,
        const Option<process::http::authentication::Principal>&
            principal) const;

    /**
     * Continuation for operations: /reserve, /unreserve,
     * /create-volumes and /destroy-volumes. First tries to recover
     * 'required' amount of resources by rescinding outstanding
     * offers, then tries to apply the operation by calling
     * 'master->apply' and propagates the 'Future<Nothing>' as
     * 'Future<Response>' where 'Nothing' -> 'OK' and Failed ->
     * 'Conflict'.
     *
     * @param slaveId The ID of the slave that the operation is
     *     updating.
     * @param operation The operation to be performed.
     *
     * @return Returns 'OK' if successful, 'BadRequest' if the
     *     operation is malformed, 'Conflict' otherwise.
     */
    process::Future<process::http::Response> _operation(
        const SlaveID& slaveId,
        const Offer::Operation& operation) const;

    // Master API handlers.

    process::Future<process::http::Response> getAgents(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getFlags(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getHealth(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getVersion(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getRoles(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMetrics(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getLoggingLevel(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> setLoggingLevel(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> listFiles(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaster(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> updateMaintenanceSchedule(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaintenanceSchedule(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaintenanceStatus(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> startMaintenance(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> stopMaintenance(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> drainAgent(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> deactivateAgent(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> reactivateAgent(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getOperations(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getTasks(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> createVolumes(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> destroyVolumes(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> growVolume(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> shrinkVolume(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> reserveResources(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> unreserveResources(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getFrameworks(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getExecutors(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getState(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    static std::function<void(JSON::ObjectWriter*)> jsonifySubscribe(
        const Master* master,
        const process::Owned<ObjectApprovers>& approvers);
    std::string serializeSubscribe(
        const process::Owned<ObjectApprovers>& approvers) const;
    process::Future<process::http::Response> subscribe(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> readFile(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> teardown(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> markAgentGone(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> _markAgentGone(
        const SlaveID& slaveId) const;

    process::Future<process::http::Response> reconcileOperations(
        Framework* framework,
        const mesos::scheduler::Call::ReconcileOperations& call,
        ContentType contentType) const;

    Master* master;

    ReadOnlyHandler readonlyHandler;

    // NOTE: The quota specific pieces of the Operator API are factored
    // out into this separate class.
    QuotaHandler quotaHandler;

    // NOTE: The weights specific pieces of the Operator API are factored
    // out into this separate class.
    WeightsHandler weightsHandler;

    // Since the Master actor is one of the most loaded in a typical Mesos
    // installation, we take some extra care to keep the backlog small.
    // In particular, all read-only requests are batched and executed in
    // parallel, instead of going through the master queue separately.
    // The post-processing step, that depends on the handler, will be
    // executed synchronously and serially after the parallel executions
    // complete.

    typedef std::pair<
        process::http::Response,
        Option<ReadOnlyHandler::PostProcessing>>
      (Master::ReadOnlyHandler::*ReadOnlyRequestHandler)(
          ContentType,
          const hashmap<std::string, std::string>&,
          const process::Owned<ObjectApprovers>&) const;

    process::Future<process::http::Response> deferBatchedRequest(
        ReadOnlyRequestHandler handler,
        const Option<process::http::authentication::Principal>& principal,
        ContentType outputContentType,
        const hashmap<std::string, std::string>& queryParameters,
        const process::Owned<ObjectApprovers>& approvers) const;

    void processRequestsBatch() const;

    struct BatchedRequest
    {
      ReadOnlyRequestHandler handler;
      ContentType outputContentType;
      hashmap<std::string, std::string> queryParameters;
      Option<process::http::authentication::Principal> principal;
      process::Owned<ObjectApprovers> approvers;
      process::Promise<process::http::Response> promise;
    };

    mutable std::vector<BatchedRequest> batchedRequests;
  };

  Master(const Master&);              // No copying.
  Master& operator=(const Master&); // No assigning.

  friend struct Framework;
  friend struct FrameworkMetrics;
  friend struct Metrics;
  friend struct Role;
  friend struct Slave;
  friend struct SlavesWriter;
  friend struct Subscriber;

  // NOTE: Since 'getOffer', 'getInverseOffer' and 'slaves' are
  // protected, we need to make the following functions friends.
  friend Offer* validation::offer::getOffer(
      Master* master, const OfferID& offerId);

  friend InverseOffer* validation::offer::getInverseOffer(
      Master* master, const OfferID& offerId);

  friend Slave* validation::offer::getSlave(
      Master* master, const SlaveID& slaveId);

  const Flags flags;

  Http http;

  Option<MasterInfo> leader; // Current leading master.

  mesos::allocator::Allocator* allocator;
  WhitelistWatcher* whitelistWatcher;
  Registrar* registrar;
  Files* files;

  mesos::master::contender::MasterContender* contender;
  mesos::master::detector::MasterDetector* detector;

  const Option<Authorizer*> authorizer;

  MasterInfo info_;

  // Holds some info which affects how a machine behaves, as well as state that
  // represent the master's view of this machine. See the `MachineInfo` protobuf
  // and `Machine` struct for more information.
  hashmap<MachineID, Machine> machines;

  struct Maintenance
  {
    // Holds the maintenance schedule, as given by the operator.
    std::list<mesos::maintenance::Schedule> schedules;
  } maintenance;

  // Indicates when recovery is complete. Recovery begins once the
  // master is elected as a leader.
  Option<process::Future<Nothing>> recovered;

  // If this is the leading master, we periodically check whether we
  // should GC some information from the registry.
  Option<process::Timer> registryGcTimer;

  struct Slaves
  {
    Slaves() : removed(MAX_REMOVED_SLAVES) {}

    // Imposes a time limit for slaves that we recover from the
    // registry to reregister with the master.
    Option<process::Timer> recoveredTimer;

    // Slaves that have been recovered from the registrar after master
    // failover. Slaves are removed from this collection when they
    // either reregister with the master or are marked unreachable
    // because they do not reregister before `recoveredTimer` fires.
    // We must not answer questions related to these slaves (e.g.,
    // during task reconciliation) until we determine their fate
    // because their are in this transitioning state.
    hashmap<SlaveID, SlaveInfo> recovered;

    // Agents that are in the process of (re-)registering. They are
    // maintained here while the (re-)registration is in progress and
    // possibly pending in the authorizer or the registrar in order
    // to help deduplicate (re-)registration requests.
    hashset<process::UPID> registering;
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
        return ids.get(slaveId).getOrElse(nullptr);
      }

      Slave* get(const process::UPID& pid) const
      {
        return pids.get(pid).getOrElse(nullptr);
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
    // registrar.
    hashset<SlaveID> removing;

    // Slaves that are in the process of being marked unreachable.
    hashset<SlaveID> markingUnreachable;

    // Slaves that are in the process of being marked gone.
    hashset<SlaveID> markingGone;

    // Agents which have been marked for draining, including recovered,
    // admitted, and unreachable agents. All draining agents will also
    // be deactivated. If an agent in this set reregisters, the master
    // will send it a `DrainSlaveMessage`.
    //
    // These values are checkpointed to the registry.
    hashmap<SlaveID, DrainInfo> draining;

    // Agents which have been deactivated, including recovered, admitted,
    // and unreachable agents. Agents in this set will not have resource
    // offers generated and will thus be unable to launch new operations,
    // but existing operations will be unaffected.
    //
    // These values are checkpointed to the registry.
    hashset<SlaveID> deactivated;

    // This collection includes agents that have gracefully shutdown,
    // as well as those that have been marked unreachable or gone. We
    // keep a cache here to prevent this from growing in an unbounded
    // manner.
    //
    // TODO(bmahler): Ideally we could use a cache with set semantics.
    //
    // TODO(neilc): Consider storing all agent IDs that have been
    // marked unreachable by this master.
    Cache<SlaveID, Nothing> removed;

    // Slaves that have been marked unreachable. We recover this from
    // the registry, so it includes slaves marked as unreachable by
    // other instances of the master. Note that we use a LinkedHashMap
    // to ensure the order of elements here matches the order in the
    // registry's unreachable list, which matches the order in which
    // agents are marked unreachable. This list is garbage collected;
    // GC behavior is governed by the `registry_gc_interval`,
    // `registry_max_agent_age`, and `registry_max_agent_count` flags.
    LinkedHashMap<SlaveID, TimeInfo> unreachable;

    // This helps us look up all unreachable tasks on an agent so we can remove
    // them from their primary storage `framework.unreachableTasks` when an
    // agent reregisters. This map is bounded by the same GC behavior as
    // `unreachable`. When the agent is GC'd from unreachable it's also
    // erased from `unreachableTasks`.
    hashmap<SlaveID, hashmap<FrameworkID, std::vector<TaskID>>>
      unreachableTasks;

    // Slaves that have been marked gone. We recover this from the
    // registry, so it includes slaves marked as gone by other instances
    // of the master. Note that we use a LinkedHashMap to ensure the order
    // of elements here matches the order in the registry's gone list, which
    // matches the order in which agents are marked gone.
    LinkedHashMap<SlaveID, TimeInfo> gone;

    // This rate limiter is used to limit the removal of slaves failing
    // health checks.
    // NOTE: Using a 'shared_ptr' here is OK because 'RateLimiter' is
    // a wrapper around libprocess process which is thread safe.
    Option<std::shared_ptr<process::RateLimiter>> limiter;
  } slaves;

  struct Frameworks
  {
    Frameworks(const Flags& masterFlags)
      : completed(masterFlags.max_completed_frameworks) {}

    hashmap<FrameworkID, Framework*> registered;

    BoundedHashMap<FrameworkID, process::Owned<Framework>> completed;

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

  struct Subscribers
  {
    Subscribers(Master* _master, size_t maxSubscribers)
      : master(_master),
        subscribed(maxSubscribers) {};

    // Represents a client subscribed to the 'api/vX' endpoint.
    //
    // TODO(anand): Add support for filtering. Some subscribers
    // might only be interested in a subset of events.
    struct Subscriber
    {
      Subscriber(
          const StreamingHttpConnection<v1::master::Event>& _http,
          const process::Owned<ObjectApprovers>& _approvers)
        : http(_http),
          heartbeater(
              "subscriber " + stringify(http.streamId),
              []() {
                mesos::master::Event event;
                event.set_type(mesos::master::Event::HEARTBEAT);
                return event;
              }(),
              http,
              DEFAULT_HEARTBEAT_INTERVAL,
              DEFAULT_HEARTBEAT_INTERVAL),
          approvers(_approvers) {}


      // Not copyable, not assignable.
      Subscriber(const Subscriber&) = delete;
      Subscriber& operator=(const Subscriber&) = delete;

      // TODO(greggomann): Refactor this function into multiple event-specific
      // overloads. See MESOS-8475.
      void send(
          const mesos::master::Event& event,
          const Option<FrameworkInfo>& frameworkInfo,
          const Option<Task>& task);

      ~Subscriber()
      {
        // TODO(anand): Refactor `HttpConnection` to being a RAII class instead.
        // It is possible that a caller might accidentally invoke `close()`
        // after passing ownership to the `Subscriber` object. See MESOS-5843
        // for more details.
        http.close();
      }

      StreamingHttpConnection<v1::master::Event> http;
      ResponseHeartbeater<mesos::master::Event, v1::master::Event> heartbeater;
      const process::Owned<ObjectApprovers> approvers;
    };

    // Sends the event to all subscribers connected to the 'api/vX' endpoint.
    void send(
        const mesos::master::Event& event,
        const Option<FrameworkInfo>& frameworkInfo = None(),
        const Option<Task>& task = None());

    Master* master;

    // Active subscribers to the 'api/vX' endpoint keyed by the stream
    // identifier.
    BoundedHashMap<id::UUID, process::Owned<Subscriber>> subscribed;
  };

  Subscribers subscribers;

  hashmap<OfferID, Offer*> offers;
  hashmap<OfferID, process::Timer> offerTimers;

  hashmap<OfferID, InverseOffer*> inverseOffers;
  hashmap<OfferID, process::Timer> inverseOfferTimers;

  // We track information about roles that we're aware of in the system.
  // Specifically, we keep track of the roles when a framework subscribes to
  // the role, and/or when there are resources allocated to the role
  // (e.g. some tasks and/or executors are consuming resources under the role).
  hashmap<std::string, Role*> roles;

  // Configured role whitelist if using the (deprecated) "explicit
  // roles" feature. If this is `None`, any role is allowed.
  Option<hashset<std::string>> roleWhitelist;

  // Configured weight for each role, if any. If a role does not
  // appear here, it has the default weight of 1.
  hashmap<std::string, double> weights;

  // Configured quota for each role, if any. We store quotas by role
  // because we set them at the role level.
  hashmap<std::string, Quota> quotas;

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

  // PullGauge handlers.
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
  double _slaves_unreachable();

  // TODO(bevers): Remove these and make the above functions
  // const instead after MESOS-4995 is resolved.
  double _const_slaves_connected() const;
  double _const_slaves_disconnected() const;
  double _const_slaves_active() const;
  double _const_slaves_inactive() const;
  double _const_slaves_unreachable() const;

  double _frameworks_connected();
  double _frameworks_disconnected();
  double _frameworks_active();
  double _frameworks_inactive();

  double _outstanding_offers()
  {
    return static_cast<double>(offers.size());
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
  double _tasks_unreachable();
  double _tasks_killing();

  double _resources_total(const std::string& name);
  double _resources_used(const std::string& name);
  double _resources_percent(const std::string& name);

  double _resources_revocable_total(const std::string& name);
  double _resources_revocable_used(const std::string& name);
  double _resources_revocable_percent(const std::string& name);

  process::Time startTime; // Start time used to calculate uptime.

  Option<process::Time> electedTime; // Time when this master is elected.

  ::mesos::allocator::OfferConstraintsFilter::Options
    offerConstraintsFilterOptions;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework);


// TODO(bmahler): Keeping the task and executor information in sync
// across the Slave and Framework structs is error prone!
struct Framework
{
  enum State
  {
    // Framework has never connected to this master. This implies the
    // master failed over and the framework has not yet reregistered,
    // but some framework state has been recovered from reregistering
    // agents that are running tasks for the framework.
    RECOVERED,

    // The framework is connected. The framework may or may not be eligible to
    // receive offers; this property is tracked separately.
    CONNECTED,

    // Framework was previously connected to this master,
    // but is not connected now.
    DISCONNECTED
  };

  Framework(
      Master* const master,
      const Flags& masterFlags,
      const FrameworkInfo& info,
      ::mesos::scheduler::OfferConstraints&& offerConstraints,
      const process::UPID& _pid,
      const process::Owned<ObjectApprovers>& objectApprovers,
      const process::Time& time = process::Clock::now());

  Framework(Master* const master,
            const Flags& masterFlags,
            const FrameworkInfo& info,
            ::mesos::scheduler::OfferConstraints&& offerConstraints,
            const StreamingHttpConnection<v1::scheduler::Event>& _http,
            const process::Owned<ObjectApprovers>& objectApprovers,
            const process::Time& time = process::Clock::now());

  Framework(Master* const master,
            const Flags& masterFlags,
            const FrameworkInfo& info);

  ~Framework();

  Task* getTask(const TaskID& taskId);

  void addTask(Task* task);

  // Update framework to recover the resources that were previously
  // being used by `task`.
  //
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void recoverResources(Task* task);

  // Sends a message to the connected framework.
  template <typename Message>
  void send(const Message& message);

  void addCompletedTask(Task&& task);

  void addUnreachableTask(const Task& task);

  // Removes the task. `unreachable` indicates whether the task is removed due
  // to being unreachable. Note that we cannot rely on the task state because
  // it may not reflect unreachability due to being set to TASK_LOST for
  // backwards compatibility.
  void removeTask(Task* task, bool unreachable);

  void addOffer(Offer* offer);

  void removeOffer(Offer* offer);

  void addInverseOffer(InverseOffer* inverseOffer);

  void removeInverseOffer(InverseOffer* inverseOffer);

  bool hasExecutor(const SlaveID& slaveId,
                   const ExecutorID& executorId);

  void addExecutor(const SlaveID& slaveId,
                   const ExecutorInfo& executorInfo);

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId);

  void addOperation(Operation* operation);

  Option<Operation*> getOperation(const OperationID& id);

  void recoverResources(Operation* operation);

  void removeOperation(Operation* operation);

  const FrameworkID id() const;

  // Update fields in 'info' using those in 'newInfo'. Currently this
  // only updates `role`/`roles`, 'name', 'failover_timeout', 'hostname',
  // 'webui_url', 'capabilities', and 'labels'.
  void update(
      const FrameworkInfo& newInfo,
      ::mesos::scheduler::OfferConstraints&& offerConstraints);

  // Reactivate framework with new connection: update connection-related state
  // and mark the framework as CONNECTED, regardless of the previous state.
  void updateConnection(
      const process::UPID& newPid,
      const process::Owned<ObjectApprovers>& objectApprovers);

  void updateConnection(
      const StreamingHttpConnection<v1::scheduler::Event>& newHttp,
      const process::Owned<ObjectApprovers>& objectApprovers);

  // If the framework is CONNECTED, clear all state associated with
  // the scheduler being connected (close http connection, stop heartbeater,
  // clear object approvers, etc.), mark the framework DISCONNECTED and return
  // `true`. Otherwise, return `false`.
  bool disconnect();

  // Mark the framework as active (eligible to receive offers if connected)
  // or inactive. Returns true if this property changed, false otherwise.
  bool activate();
  bool deactivate();

  void heartbeat();

  bool active() const { return active_; }

  bool connected() const {return state == State::CONNECTED;}
  bool recovered() const {return state == State::RECOVERED;}

  bool isTrackedUnderRole(const std::string& role) const;
  void trackUnderRole(const std::string& role);
  void untrackUnderRole(const std::string& role);

  const Option<StreamingHttpConnection<v1::scheduler::Event>>& http() const
  {
    return http_;
  }

  const Option<process::UPID>& pid() const { return pid_; }

  // Returns ObjectApprovers for all actions
  // needed to authorize scheduler API calls.
  static process::Future<process::Owned<ObjectApprovers>> createObjectApprovers(
      const Option<Authorizer*>& _authorizer,
      const FrameworkInfo& frameworkInfo);

  // Returns whether the framework principal is authorized to perform
  // action on object.
  Try<bool> approved(const authorization::ActionObject& actionObject) const;

  const ::mesos::scheduler::OfferConstraints& offerConstraints() const
  {
    return offerConstraints_;
  }

  Master* const master;

  FrameworkInfo info;

  std::set<std::string> roles;

  protobuf::framework::Capabilities capabilities;

  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // TODO(bmahler): Make this private to enforce that `addTask()` and
  // `removeTask()` are used, and provide a const view into the tasks.
  hashmap<TaskID, Task*> tasks;

  // Tasks launched by this framework that have reached a terminal
  // state and have had all their updates acknowledged. We only keep a
  // fixed-size cache to avoid consuming too much memory. We use
  // circular_buffer rather than BoundedHashMap because there
  // can be multiple completed tasks with the same task ID.
  circular_buffer<process::Owned<Task>> completedTasks;

  // When an agent is marked unreachable, tasks running on it are stored
  // here. We only keep a fixed-size cache to avoid consuming too much memory.
  // NOTE: Non-partition-aware unreachable tasks in this map are marked
  // TASK_LOST instead of TASK_UNREACHABLE for backward compatibility.
  BoundedHashMap<TaskID, process::Owned<Task>> unreachableTasks;

  hashset<Offer*> offers; // Active offers for framework.

  hashset<InverseOffer*> inverseOffers; // Active inverse offers for framework.

  // TODO(bmahler): Make this private to enforce that `addExecutor()`
  // and `removeExecutor()` are used, and provide a const view into
  // the executors.
  hashmap<SlaveID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // Pending operations or terminal operations that have
  // unacknowledged status updates.
  hashmap<UUID, Operation*> operations;

  // The map from the framework-specified operation ID to the
  // corresponding internal operation UUID.
  hashmap<OperationID, UUID> operationUUIDs;

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

  // Active task / executor / operation resources.
  Resources totalUsedResources;

  // Note that we maintain multiple copies of each shared resource in
  // `usedResources` as they are used by multiple tasks.
  hashmap<SlaveID, Resources> usedResources;

  // Offered resources.
  Resources totalOfferedResources;
  hashmap<SlaveID, Resources> offeredResources;

  // This is used for per-framework metrics.
  FrameworkMetrics metrics;

private:
  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            ::mesos::scheduler::OfferConstraints&& offerConstraints,
            State state,
            bool active,
            const process::Owned<ObjectApprovers>& objectApprovers,
            const process::Time& time);

  Framework(const Framework&);              // No copying.
  Framework& operator=(const Framework&); // No assigning.

  // Indicates whether this framework should be receiving offers
  // when it is connected.
  bool active_;

  // NOTE: `state` should never modified by means other than `setState()`.
  //
  // TODO(asekretenko): Encapsulate `state` to ensure that `metrics.subscribed`
  // is updated together with any `state` change.
  State state;

  void setState(State state_);

  // Frameworks can either be connected via HTTP or by message passing
  // (scheduler driver). At most one of `http` and `pid` will be set
  // according to the last connection made by the framework; neither
  // field will be set if the framework is in state `RECOVERED`.
  Option<StreamingHttpConnection<v1::scheduler::Event>> http_;
  Option<process::UPID> pid_;

  // This is only set for HTTP frameworks.
  process::Owned<ResponseHeartbeater<scheduler::Event, v1::scheduler::Event>>
    heartbeater;

  // ObjectApprovers for the framework's principal.
  process::Owned<ObjectApprovers> objectApprovers;

  // The last offer constraints with which the framework has been subscribed.
  ::mesos::scheduler::OfferConstraints offerConstraints_;
};


// Sends a message to the connected framework.
template <typename Message>
void Framework::send(const Message& message)
{
  metrics.incrementEvent(message);

  if (!connected()) {
    LOG(WARNING) << "Master attempting to send message to disconnected"
                 << " framework " << *this;

    // NOTE: We proceed here without returning to support the case where a
    // "disconnected" framework is still talking to the master and the master
    // wants to shut it down by sending a `FrameworkErrorMessage`. This can
    // occur in a one-way network partition where the master -> framework link
    // is broken but the framework -> master link remains intact. Note that we
    // have no periodic heartbeats between the master and pid-based schedulers.
    //
    // TODO(chhsiao): Update the `FrameworkErrorMessage` call-sites that rely on
    // the lack of a `return` here to directly call `process::send` so that this
    // function doesn't need to deal with the special case. Then we can check
    // that one of `http` or `pid` is set if the framework is connected.
  }

  if (http_.isSome()) {
    if (!http_->send(message)) {
      LOG(WARNING) << "Unable to send message to framework " << *this << ":"
                   << " connection closed";
    }
  } else if (pid().isSome()) {
    master->send(pid().get(), message);
  } else {
    LOG(WARNING) << "Unable to send message to framework " << *this << ":"
                 << " framework is recovered but has not reregistered";
  }
}


// TODO(bevers): Check if there is anything preventing us from
// returning a const reference here.
inline const FrameworkID Framework::id() const
{
  return info.id();
}



inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework)
{
  // TODO(vinod): Also log the hostname once FrameworkInfo is properly
  // updated on framework failover (MESOS-1784).
  stream << framework.id() << " (" << framework.info.name() << ")";

  if (framework.pid().isSome()) {
    stream << " at " << framework.pid().get();
  }

  return stream;
}


// Information about an active role.
struct Role
{
  Role() = delete;

  Role(const Master* _master,
       const std::string& _role)
    : master(_master), role(_role) {}

  void addFramework(Framework* framework)
  {
    frameworks[framework->id()] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id());
  }

  const Master* master;
  const std::string role;

  // NOTE: The dynamic role/quota relation is stored in and administrated
  // by the master. There is no direct representation of quota information
  // here to avoid duplication and to support that an operator can associate
  // quota with a role before the role is created. Such ordering of operator
  // requests prevents a race of premature unbounded allocation that setting
  // quota first is intended to contain.

  hashmap<FrameworkID, Framework*> frameworks;
};


mesos::master::Response::GetFrameworks::Framework model(
    const Framework& framework);


} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
