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

#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/master/contender.hpp>
#include <mesos/master/detector.hpp>
#include <mesos/master/master.hpp>

#include <mesos/module/authenticator.hpp>

#include <mesos/quota/quota.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/limiter.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>

#include <stout/boundedhashmap.hpp>
#include <stout/cache.hpp>
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

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "files/files.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

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

  // Slave becomes deactivated when it gets disconnected. In the
  // future this might also happen via HTTP endpoint.
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

  // Tasks that have not yet been launched because they are currently
  // being authorized. This is similar to Framework's pendingTasks but we
  // track pendingTasks per agent separately to determine if any offer
  // operation for this agent would change resources requested by these tasks.
  hashmap<FrameworkID, hashmap<TaskID, TaskInfo>> pendingTasks;

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


// Represents the streaming HTTP connection to a framework or a client
// subscribed to the '/api/vX' endpoint.
struct HttpConnection
{
  HttpConnection(const process::http::Pipe::Writer& _writer,
                 ContentType _contentType,
                 id::UUID _streamId)
    : writer(_writer),
      contentType(_contentType),
      streamId(_streamId) {}

  // We need to evolve the internal old style message/unversioned event into a
  // versioned event e.g., `v1::scheduler::Event` or `v1::master::Event`.
  template <typename Message, typename Event = v1::scheduler::Event>
  bool send(const Message& message)
  {
    ::recordio::Encoder<Event> encoder (lambda::bind(
        serialize, contentType, lambda::_1));

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
  id::UUID streamId;
};


// This process periodically sends heartbeats to a given HTTP connection.
// The `Message` template parameter is the type of the heartbeat event passed
// into the heartbeater during construction, while the `Event` template
// parameter is the versioned event type which is sent to the client.
// The optional delay parameter is used to specify the delay period before it
// sends the first heartbeat.
template <typename Message, typename Event>
class Heartbeater : public process::Process<Heartbeater<Message, Event>>
{
public:
  Heartbeater(const std::string& _logMessage,
              const Message& _heartbeatMessage,
              const HttpConnection& _http,
              const Duration& _interval,
              const Option<Duration>& _delay = None())
    : process::ProcessBase(process::ID::generate("heartbeater")),
      logMessage(_logMessage),
      heartbeatMessage(_heartbeatMessage),
      http(_http),
      interval(_interval),
      delay(_delay) {}

protected:
  void initialize() override
  {
    if (delay.isSome()) {
      process::delay(
          delay.get(),
          this,
          &Heartbeater<Message, Event>::heartbeat);
    } else {
      heartbeat();
    }
  }

private:
  void heartbeat()
  {
    // Only send a heartbeat if the connection is not closed.
    if (http.closed().isPending()) {
      VLOG(2) << "Sending heartbeat to " << logMessage;

      Message message(heartbeatMessage);
      http.send<Message, Event>(message);
    }

    process::delay(interval, this, &Heartbeater<Message, Event>::heartbeat);
  }

  const std::string logMessage;
  const Message heartbeatMessage;
  HttpConnection http;
  const Duration interval;
  const Option<Duration> delay;
};


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

  void markGone(Slave* slave, const TimeInfo& goneTime);

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
  void exited(const FrameworkID& frameworkId, const HttpConnection& http);
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
      const std::set<std::string>& suppressedRoles);

  // Recover a framework from its `FrameworkInfo`. This happens after
  // master failover, when an agent running one of the framework's
  // tasks reregisters or when the framework itself reregisters,
  // whichever happens first. The result of this function is a
  // registered, inactive framework with state `RECOVERED`.
  void recoverFramework(
      const FrameworkInfo& info,
      const std::set<std::string>& suppressedRoles);

  // Transition a framework from `RECOVERED` to `CONNECTED` state and
  // activate it. This happens at most once after master failover, the
  // first time that the framework reregisters with the new master.
  // Exactly one of `newPid` or `http` must be provided.
  Try<Nothing> activateRecoveredFramework(
      Framework* framework,
      const FrameworkInfo& frameworkInfo,
      const Option<process::UPID>& pid,
      const Option<HttpConnection>& http,
      const std::set<std::string>& suppressedRoles);

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

  void updateFramework(
      Framework* framework,
      const FrameworkInfo& frameworkInfo,
      const std::set<std::string>& suppressedRoles);

  void disconnect(Framework* framework);
  void deactivate(Framework* framework, bool rescind);

  void disconnect(Slave* slave);
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

  void _removeSlave(
      Slave* slave,
      const process::Future<bool>& registrarResult,
      const std::string& removalCause,
      Option<process::metrics::Counter> reason = None());

  void __removeSlave(
      Slave* slave,
      const std::string& message,
      const Option<TimeInfo>& unreachableTime);

  // Validates that the framework is authenticated, if required.
  Option<Error> validateFrameworkAuthentication(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  // Returns whether the framework is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeFramework(
      const FrameworkInfo& frameworkInfo);

  // Returns whether the principal is authorized to (re-)register an agent
  // and whether the `SlaveInfo` is authorized.
  process::Future<bool> authorizeSlave(
      const SlaveInfo& slaveInfo,
      const Option<process::http::authentication::Principal>& principal);

  // Returns whether the task is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeTask(
      const TaskInfo& task,
      Framework* framework);

  /**
   * Authorizes a `RESERVE` operation.
   *
   * Returns whether the Reserve operation is authorized with the
   * provided principal. This function is used for authorization of
   * operations originating from both frameworks and operators. Note
   * that operations may be validated AFTER authorization, so it's
   * possible that `reserve` could be malformed.
   *
   * @param reserve The `RESERVE` operation to be performed.
   * @param principal An `Option` containing the principal attempting
   *     this operation.
   *
   * @return A `Future` containing a boolean value representing the
   *     success or failure of this authorization. A failed `Future`
   *     implies that validation of the operation did not succeed.
   */
  process::Future<bool> authorizeReserveResources(
      const Offer::Operation::Reserve& reserve,
      const Option<process::http::authentication::Principal>& principal);

  // Authorizes whether the provided `principal` is allowed to reserve
  // the specified `resources`.
  process::Future<bool> authorizeReserveResources(
      const Resources& resources,
      const Option<process::http::authentication::Principal>& principal);

  /**
   * Authorizes an `UNRESERVE` operation.
   *
   * Returns whether the Unreserve operation is authorized with the
   * provided principal. This function is used for authorization of
   * operations originating both from frameworks and operators. Note
   * that operations may be validated AFTER authorization, so it's
   * possible that `unreserve` could be malformed.
   *
   * @param unreserve The `UNRESERVE` operation to be performed.
   * @param principal An `Option` containing the principal attempting
   *     this operation.
   *
   * @return A `Future` containing a boolean value representing the
   *     success or failure of this authorization. A failed `Future`
   *     implies that validation of the operation did not succeed.
   */
  process::Future<bool> authorizeUnreserveResources(
      const Offer::Operation::Unreserve& unreserve,
      const Option<process::http::authentication::Principal>& principal);

  /**
   * Authorizes a `CREATE` operation.
   *
   * Returns whether the Create operation is authorized with the provided
   * principal. This function is used for authorization of operations
   * originating both from frameworks and operators. Note that operations may be
   * validated AFTER authorization, so it's possible that `create` could be
   * malformed.
   *
   * @param create The `CREATE` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeCreateVolume(
      const Offer::Operation::Create& create,
      const Option<process::http::authentication::Principal>& principal);

  /**
   * Authorizes a `DESTROY` operation.
   *
   * Returns whether the Destroy operation is authorized with the provided
   * principal. This function is used for authorization of operations
   * originating both from frameworks and operators. Note that operations may be
   * validated AFTER authorization, so it's possible that `destroy` could be
   * malformed.
   *
   * @param destroy The `DESTROY` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeDestroyVolume(
      const Offer::Operation::Destroy& destroy,
      const Option<process::http::authentication::Principal>& principal);

  /**
   * Authorizes resize of a volume triggered by either `GROW_VOLUME` or
   * `SHRINK_VOLUME` operations.
   *
   * Returns whether the triggering operation is authorized with the provided
   * principal. This function is used for authorization of operations
   * originating both from frameworks and operators. Note that operations may be
   * validated AFTER authorization, so it's possible that the operation could be
   * malformed.
   *
   * @param volume The volume being resized.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeResizeVolume(
      const Resource& volume,
      const Option<process::http::authentication::Principal>& principal);


  /**
   * Authorizes a `CREATE_DISK` operation.
   *
   * Returns whether the `CREATE_DISK` operation is authorized with the
   * provided principal. This function is used for authorization of operations
   * originating from frameworks. Note that operations may be validated AFTER
   * authorization, so it's possible that the operation could be malformed.
   *
   * @param createDisk The `CREATE_DISK` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeCreateDisk(
      const Offer::Operation::CreateDisk& createDisk,
      const Option<process::http::authentication::Principal>& principal);


  /**
   * Authorizes a `DESTROY_DISK` operation.
   *
   * Returns whether the `DESTROY_DISK` operation is authorized with the
   * provided principal. This function is used for authorization of operations
   * originating from frameworks. Note that operations may be validated AFTER
   * authorization, so it's possible that the operation could be malformed.
   *
   * @param destroyDisk The `DESTROY_DISK` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeDestroyDisk(
      const Offer::Operation::DestroyDisk& destroyDisk,
      const Option<process::http::authentication::Principal>& principal);


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

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  // Remove an inverse offer after specified timeout
  void inverseOfferTimeout(const OfferID& inverseOfferId);

  // Remove an inverse offer and optionally rescind it as well.
  void removeInverseOffer(InverseOffer* inverseOffer, bool rescind = false);

  bool isCompletedFramework(const FrameworkID& frameworkId);

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
      const scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const Offer::Operation& operation,
      const std::string& message);

  void drop(
      Framework* framework,
      const scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const scheduler::Call::Suppress& suppress,
      const std::string& message);

  void drop(
      Framework* framework,
      const scheduler::Call::Revive& revive,
      const std::string& message);

  // Call handlers.
  void receive(
      const process::UPID& from,
      scheduler::Call&& call);

  void subscribe(
      HttpConnection http,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      HttpConnection http,
      const FrameworkInfo& frameworkInfo,
      bool force,
      const std::set<std::string>& suppressedRoles,
      const process::Future<bool>& authorized);

  void subscribe(
      const process::UPID& from,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool force,
      const std::set<std::string>& suppressedRoles,
      const process::Future<bool>& authorized);

  // Subscribes a client to the 'api/vX' endpoint.
  void subscribe(
      const HttpConnection& http,
      const Option<process::http::authentication::Principal>& principal);

  void teardown(Framework* framework);

  void accept(
      Framework* framework,
      scheduler::Call::Accept&& accept);

  void _accept(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& offeredResources,
      scheduler::Call::Accept&& accept,
      const process::Future<
          std::vector<process::Future<bool>>>& authorizations);

  void acceptInverseOffers(
      Framework* framework,
      const scheduler::Call::AcceptInverseOffers& accept);

  void decline(
      Framework* framework,
      scheduler::Call::Decline&& decline);

  void declineInverseOffers(
      Framework* framework,
      const scheduler::Call::DeclineInverseOffers& decline);

  void revive(
      Framework* framework,
      const scheduler::Call::Revive& revive);

  void kill(
      Framework* framework,
      const scheduler::Call::Kill& kill);

  void shutdown(
      Framework* framework,
      const scheduler::Call::Shutdown& shutdown);

  void acknowledge(
      Framework* framework,
      scheduler::Call::Acknowledge&& acknowledge);

  void acknowledgeOperationStatus(
      Framework* framework,
      scheduler::Call::AcknowledgeOperationStatus&& acknowledge);

  void reconcile(
      Framework* framework,
      scheduler::Call::Reconcile&& reconcile);

  scheduler::Response::ReconcileOperations reconcileOperations(
      Framework* framework,
      const scheduler::Call::ReconcileOperations& reconcile);

  void message(
      Framework* framework,
      scheduler::Call::Message&& message);

  void request(
      Framework* framework,
      const scheduler::Call::Request& request);

  void suppress(
      Framework* framework,
      const scheduler::Call::Suppress& suppress);

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

  process::Future<bool> authorizeLogAccess(
      const Option<process::http::authentication::Principal>& principal);

  /**
   * Returns whether the given role is on the whitelist.
   *
   * When using explicit roles, this consults the configured (static)
   * role whitelist. When using implicit roles, any role is allowed
   * (and access control is done via ACLs).
   */
  bool isWhitelistedRole(const std::string& name) const;

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
    // Heuristically tries to determine whether a quota request could
    // reasonably be satisfied given the current cluster capacity. The
    // goal is to determine whether a user may accidentally request an
    // amount of resources that would prevent frameworks without quota
    // from getting any offers. A force flag will allow users to bypass
    // this check.
    //
    // The heuristic tests whether the total quota, including the new
    // request, does not exceed the sum of non-static cluster resources,
    // i.e. the following inequality holds:
    //   total - statically reserved >= total quota + quota request
    //
    // Please be advised that:
    //   * It is up to an allocator how to satisfy quota (for example,
    //     what resources to account towards quota, as well as which
    //     resources to consider allocatable for quota).
    //   * Even if there are enough resources at the moment of this check,
    //     agents may terminate at any time, rendering the cluster under
    //     quota.
    Option<Error> capacityHeuristic(
        const mesos::quota::QuotaInfo& request) const;

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
        const mesos::quota::QuotaInfo& quotaInfo) const;

    process::Future<bool> authorizeUpdateQuota(
        const Option<process::http::authentication::Principal>& principal,
        const mesos::quota::QuotaInfo& quotaInfo) const;

    process::Future<mesos::quota::QuotaStatus> _status(
        const Option<process::http::authentication::Principal>&
            principal) const;

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

  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master),
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
    process::Future<process::http::Response> slaves(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/state
    process::Future<process::http::Response> state(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/state-summary
    process::Future<process::http::Response> stateSummary(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/tasks
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

    // /master/quota
    process::Future<process::http::Response> quota(
        const process::http::Request& request,
        const Option<process::http::authentication::Principal>&
            principal) const;

    // /master/weights
    process::Future<process::http::Response> weights(
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

    process::Future<process::http::Response> _reserve(
        const SlaveID& slaveId,
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
     * @param required The resources needed to satisfy the operation.
     *     This is used for an optimization where we try to only
     *     rescind offers that would contribute to satisfying the
     *     operation.
     * @param operation The operation to be performed.
     *
     * @return Returns 'OK' if successful, 'Conflict' otherwise.
     */
    process::Future<process::http::Response> _operation(
        const SlaveID& slaveId,
        Resources required,
        const Offer::Operation& operation) const;

    process::Future<std::vector<std::string>> _roles(
        const Option<process::http::authentication::Principal>&
            principal) const;

    // Master API handlers.

    process::Future<process::http::Response> getAgents(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetAgents _getAgents(
        const process::Owned<ObjectApprovers>& approvers) const;

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

    process::Future<process::http::Response> getOperations(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getTasks(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetTasks _getTasks(
        const process::Owned<ObjectApprovers>& approvers) const;

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

    mesos::master::Response::GetFrameworks _getFrameworks(
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> getExecutors(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetExecutors _getExecutors(
        const process::Owned<ObjectApprovers>& approvers) const;

    process::Future<process::http::Response> getState(
        const mesos::master::Call& call,
        const Option<process::http::authentication::Principal>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetState _getState(
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
        const scheduler::Call::ReconcileOperations& call,
        ContentType contentType) const;

    Master* master;

    // NOTE: The quota specific pieces of the Operator API are factored
    // out into this separate class.
    QuotaHandler quotaHandler;

    // NOTE: The weights specific pieces of the Operator API are factored
    // out into this separate class.
    WeightsHandler weightsHandler;
  };

  Master(const Master&);              // No copying.
  Master& operator=(const Master&); // No assigning.

  friend struct Framework;
  friend struct Metrics;
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
    hashmap<SlaveID, multihashmap<FrameworkID, TaskID>> unreachableTasks;

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
    Subscribers(Master* _master) : master(_master) {};

    // Represents a client subscribed to the 'api/vX' endpoint.
    //
    // TODO(anand): Add support for filtering. Some subscribers
    // might only be interested in a subset of events.
    struct Subscriber
    {
      Subscriber(
          const HttpConnection& _http,
          const Option<process::http::authentication::Principal> _principal)
        : http(_http),
          principal(_principal)
      {
        mesos::master::Event event;
        event.set_type(mesos::master::Event::HEARTBEAT);

        heartbeater =
          process::Owned<Heartbeater<mesos::master::Event, v1::master::Event>>(
              new Heartbeater<mesos::master::Event, v1::master::Event>(
                  "subscriber " + stringify(http.streamId),
                  event,
                  http,
                  DEFAULT_HEARTBEAT_INTERVAL,
                  DEFAULT_HEARTBEAT_INTERVAL));

        process::spawn(heartbeater.get());
      }

      // Not copyable, not assignable.
      Subscriber(const Subscriber&) = delete;
      Subscriber& operator=(const Subscriber&) = delete;

      // TODO(greggomann): Refactor this function into multiple event-specific
      // overloads. See MESOS-8475.
      void send(
          const process::Shared<mesos::master::Event>& event,
          const process::Owned<ObjectApprovers>& approvers,
          const process::Shared<FrameworkInfo>& frameworkInfo,
          const process::Shared<Task>& task);

      ~Subscriber()
      {
        // TODO(anand): Refactor `HttpConnection` to being a RAII class instead.
        // It is possible that a caller might accidentally invoke `close()`
        // after passing ownership to the `Subscriber` object. See MESOS-5843
        // for more details.
        http.close();

        terminate(heartbeater.get());
        wait(heartbeater.get());
      }

      HttpConnection http;
      process::Owned<Heartbeater<mesos::master::Event, v1::master::Event>>
        heartbeater;
      const Option<process::http::authentication::Principal> principal;
    };

    // Sends the event to all subscribers connected to the 'api/vX' endpoint.
    void send(
        mesos::master::Event&& event,
        const Option<FrameworkInfo>& frameworkInfo = None(),
        const Option<Task>& task = None());

    Master* master;

    // Active subscribers to the 'api/vX' endpoint keyed by the stream
    // identifier.
    hashmap<id::UUID, process::Owned<Subscriber>> subscribed;
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

  // Validates the framework including authorization.
  // Returns None if the framework is valid.
  // Returns Error if the framework is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error>> validate(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);
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

    // Framework was previously connected to this master. A framework
    // becomes disconnected when there is a socket error.
    DISCONNECTED,

    // The framework is connected but not active.
    INACTIVE,

    // Framework is connected and eligible to receive offers. No
    // offers will be made to frameworks that are not active.
    ACTIVE
  };

  Framework(Master* const master,
            const Flags& masterFlags,
            const FrameworkInfo& info,
            const process::UPID& _pid,
            const process::Time& time = process::Clock::now())
    : Framework(master, masterFlags, info, ACTIVE, time)
  {
    pid = _pid;
  }

  Framework(Master* const master,
            const Flags& masterFlags,
            const FrameworkInfo& info,
            const HttpConnection& _http,
            const process::Time& time = process::Clock::now())
    : Framework(master, masterFlags, info, ACTIVE, time)
  {
    http = _http;
  }

  Framework(Master* const master,
            const Flags& masterFlags,
            const FrameworkInfo& info)
    : Framework(master, masterFlags, info, RECOVERED, process::Time()) {}

  ~Framework()
  {
    if (http.isSome()) {
      closeHttpConnection();
    }
  }

  Task* getTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    }

    return nullptr;
  }

  void addTask(Task* task)
  {
    CHECK(!tasks.contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    // Verify that Resource.AllocationInfo is set,
    // this should be guaranteed by the master.
    foreach (const Resource& resource, task->resources()) {
      CHECK(resource.has_allocation_info());
    }

    tasks[task->task_id()] = task;

    // Unreachable tasks should be added via `addUnreachableTask`.
    CHECK(task->state() != TASK_UNREACHABLE)
      << "Task '" << task->task_id() << "' of framework " << id()
      << " added in TASK_UNREACHABLE state";

    // Since we track terminal but unacknowledged tasks within
    // `tasks` rather than `completedTasks`, we need to handle
    // them here: don't count them as consuming resources.
    //
    // TODO(bmahler): Users currently get confused because
    // terminal tasks can show up as "active" tasks in the UI and
    // endpoints. Ideally, we show the terminal unacknowledged
    // tasks as "completed" as well.
    if (!protobuf::isTerminalState(task->state())) {
      // Note that we explicitly convert from protobuf to `Resources` once
      // and then use the result for calculations to avoid performance penalty
      // for multiple conversions and validations implied by `+=` with protobuf
      // arguments.
      // Conversion is safe, as resources have already passed validation.
      const Resources resources = task->resources();
      totalUsedResources += resources;
      usedResources[task->slave_id()] += resources;

      // It's possible that we're not tracking the task's role for
      // this framework if the role is absent from the framework's
      // set of roles. In this case, we track the role's allocation
      // for this framework.
      CHECK(!task->resources().empty());
      const std::string& role =
        task->resources().begin()->allocation_info().role();

      if (!isTrackedUnderRole(role)) {
        trackUnderRole(role);
      }
    }

    if (!master->subscribers.subscribed.empty()) {
      master->subscribers.send(
          protobuf::master::event::createTaskAdded(*task),
          info);
    }
  }

  // Update framework to recover the resources that were previously
  // being used by `task`.
  //
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void recoverResources(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    totalUsedResources -= task->resources();
    usedResources[task->slave_id()] -= task->resources();
    if (usedResources[task->slave_id()].empty()) {
      usedResources.erase(task->slave_id());
    }

    // If we are no longer subscribed to the role to which these resources are
    // being returned to, and we have no more resources allocated to us for that
    // role, stop tracking the framework under the role.
    CHECK(!task->resources().empty());
    const std::string& role =
      task->resources().begin()->allocation_info().role();

    auto allocatedToRole = [&role](const Resource& resource) {
      return resource.allocation_info().role() == role;
    };

    if (roles.count(role) == 0 &&
        totalUsedResources.filter(allocatedToRole).empty()) {
      CHECK(totalOfferedResources.filter(allocatedToRole).empty());
      untrackUnderRole(role);
    }
  }

  // Sends a message to the connected framework.
  template <typename Message>
  void send(const Message& message)
  {
    if (!connected()) {
      LOG(WARNING) << "Master attempted to send message to disconnected"
                   << " framework " << *this;
    }

    if (http.isSome()) {
      if (!http->send(message)) {
        LOG(WARNING) << "Unable to send event to framework " << *this << ":"
                     << " connection closed";
      }
    } else {
      CHECK_SOME(pid);
      master->send(pid.get(), message);
    }
  }

  void addCompletedTask(Task&& task)
  {
    // TODO(neilc): We currently allow frameworks to reuse the task
    // IDs of completed tasks (although this is discouraged). This
    // means that there might be multiple completed tasks with the
    // same task ID. We should consider rejecting attempts to reuse
    // task IDs (MESOS-6779).
    completedTasks.push_back(process::Owned<Task>(new Task(std::move(task))));
  }

  void addUnreachableTask(const Task& task)
  {
    // TODO(adam-mesos): Check if unreachable task already exists.
    unreachableTasks.set(task.task_id(), process::Owned<Task>(new Task(task)));
  }

  // Removes the task. `unreachable` indicates whether the task is removed due
  // to being unreachable. Note that we cannot rely on the task state because
  // it may not reflect unreachability due to being set to TASK_LOST for
  // backwards compatibility.
  void removeTask(Task* task, bool unreachable)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    // The invariant here is that the master will have already called
    // `recoverResources()` prior to removing terminal or unreachable tasks.
    if (!protobuf::isTerminalState(task->state()) &&
        task->state() != TASK_UNREACHABLE) {
      recoverResources(task);
    }

    if (unreachable) {
      addUnreachableTask(*task);
    } else {
      CHECK(task->state() != TASK_UNREACHABLE);

      // TODO(bmahler): This moves a potentially non-terminal task into
      // the completed list!
      addCompletedTask(Task(*task));
    }

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

  void addInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(!inverseOffers.contains(inverseOffer))
      << "Duplicate inverse offer " << inverseOffer->id();
    inverseOffers.insert(inverseOffer);
  }

  void removeInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(inverseOffers.contains(inverseOffer))
      << "Unknown inverse offer " << inverseOffer->id();

    inverseOffers.erase(inverseOffer);
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
      << "Duplicate executor '" << executorInfo.executor_id()
      << "' on agent " << slaveId;

    // Verify that Resource.AllocationInfo is set,
    // this should be guaranteed by the master.
    foreach (const Resource& resource, executorInfo.resources()) {
      CHECK(resource.has_allocation_info());
    }

    executors[slaveId][executorInfo.executor_id()] = executorInfo;
    totalUsedResources += executorInfo.resources();
    usedResources[slaveId] += executorInfo.resources();

    // It's possible that we're not tracking the task's role for
    // this framework if the role is absent from the framework's
    // set of roles. In this case, we track the role's allocation
    // for this framework.
    if (!executorInfo.resources().empty()) {
      const std::string& role =
        executorInfo.resources().begin()->allocation_info().role();

      if (!isTrackedUnderRole(role)) {
        trackUnderRole(role);
      }
    }
  }

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(slaveId, executorId))
      << "Unknown executor '" << executorId
      << "' of framework " << id()
      << " of agent " << slaveId;

    const ExecutorInfo& executorInfo = executors[slaveId][executorId];

    totalUsedResources -= executorInfo.resources();
    usedResources[slaveId] -= executorInfo.resources();
    if (usedResources[slaveId].empty()) {
      usedResources.erase(slaveId);
    }

    // If we are no longer subscribed to the role to which these resources are
    // being returned to, and we have no more resources allocated to us for that
    // role, stop tracking the framework under the role.
    if (!executorInfo.resources().empty()) {
      const std::string& role =
        executorInfo.resources().begin()->allocation_info().role();

      auto allocatedToRole = [&role](const Resource& resource) {
        return resource.allocation_info().role() == role;
      };

      if (roles.count(role) == 0 &&
          totalUsedResources.filter(allocatedToRole).empty()) {
        CHECK(totalOfferedResources.filter(allocatedToRole).empty());
        untrackUnderRole(role);
      }
    }

    executors[slaveId].erase(executorId);
    if (executors[slaveId].empty()) {
      executors.erase(slaveId);
    }
  }

  void addOperation(Operation* operation)
  {
    CHECK(operation->has_framework_id());

    const FrameworkID& frameworkId = operation->framework_id();

    const UUID& uuid = operation->uuid();

    CHECK(!operations.contains(uuid))
      << "Duplicate operation '" << operation->info().id()
      << "' (uuid: " << uuid << ") "
      << "of framework " << frameworkId;

    operations.put(uuid, operation);

    if (operation->info().has_id()) {
      operationUUIDs.put(operation->info().id(), uuid);
    }

    if (!protobuf::isSpeculativeOperation(operation->info()) &&
        !protobuf::isTerminalState(operation->latest_status().state())) {
      Try<Resources> consumed =
        protobuf::getConsumedResources(operation->info());
      CHECK_SOME(consumed);

      CHECK(operation->has_slave_id())
        << "External resource provider is not supported yet";

      const SlaveID& slaveId = operation->slave_id();

      totalUsedResources += consumed.get();
      usedResources[slaveId] += consumed.get();

      // It's possible that we're not tracking the role from the
      // resources in the operation for this framework if the role is
      // absent from the framework's set of roles. In this case, we
      // track the role's allocation for this framework.
      foreachkey (const std::string& role, consumed->allocations()) {
        if (!isTrackedUnderRole(role)) {
          trackUnderRole(role);
        }
      }
    }
  }

  Option<Operation*> getOperation(const OperationID& id) {
    Option<UUID> uuid = operationUUIDs.get(id);

    if (uuid.isNone()) {
      return None();
    }

    Option<Operation*> operation = operations.get(uuid.get());

    CHECK_SOME(operation);

    return operation;
  }

  void recoverResources(Operation* operation)
  {
    CHECK(operation->has_slave_id())
      << "External resource provider is not supported yet";

    const SlaveID& slaveId = operation->slave_id();

    if (protobuf::isSpeculativeOperation(operation->info())) {
      return;
    }

    Try<Resources> consumed = protobuf::getConsumedResources(operation->info());
    CHECK_SOME(consumed);

    CHECK(totalUsedResources.contains(consumed.get()))
      << "Tried to recover resources " << consumed.get()
      << " which do not seem used";

    CHECK(usedResources[slaveId].contains(consumed.get()))
      << "Tried to recover resources " << consumed.get() << " of agent "
      << slaveId << " which do not seem used";

    totalUsedResources -= consumed.get();
    usedResources[slaveId] -= consumed.get();
    if (usedResources[slaveId].empty()) {
      usedResources.erase(slaveId);
    }

    // If we are no longer subscribed to the role to which these
    // resources are being returned to, and we have no more resources
    // allocated to us for that role, stop tracking the framework
    // under the role.
    foreachkey (const std::string& role, consumed->allocations()) {
      auto allocatedToRole = [&role](const Resource& resource) {
        return resource.allocation_info().role() == role;
      };

      if (roles.count(role) == 0 &&
          totalUsedResources.filter(allocatedToRole).empty()) {
        CHECK(totalOfferedResources.filter(allocatedToRole).empty());
        untrackUnderRole(role);
      }
    }
  }

  void removeOperation(Operation* operation)
  {
    const UUID& uuid = operation->uuid();

    CHECK(operations.contains(uuid))
      << "Unknown operation '" << operation->info().id()
      << "' (uuid: " << uuid << ") "
      << "of framework " << operation->framework_id();

    if (!protobuf::isSpeculativeOperation(operation->info()) &&
        !protobuf::isTerminalState(operation->latest_status().state())) {
      recoverResources(operation);
    }

    if (operation->info().has_id()) {
      operationUUIDs.erase(operation->info().id());
    }

    operations.erase(uuid);
  }

  const FrameworkID id() const { return info.id(); }

  // Update fields in 'info' using those in 'newInfo'. Currently this
  // only updates `role`/`roles`, 'name', 'failover_timeout', 'hostname',
  // 'webui_url', 'capabilities', and 'labels'.
  void update(const FrameworkInfo& newInfo)
  {
    // We only merge 'info' from the same framework 'id'.
    CHECK_EQ(info.id(), newInfo.id());

    // Save the old list of roles for later.
    std::set<std::string> oldRoles = roles;

    // TODO(jmlvanre): Merge other fields as per design doc in
    // MESOS-703.

    info.clear_role();
    info.clear_roles();

    if (newInfo.has_role()) {
      info.set_role(newInfo.role());
    }

    if (newInfo.roles_size() > 0) {
      info.mutable_roles()->CopyFrom(newInfo.roles());
    }

    roles = protobuf::framework::getRoles(newInfo);

    if (newInfo.user() != info.user()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.user to '" << newInfo.user()
                   << "' for framework " << id() << ". Check MESOS-703";
    }

    info.set_name(newInfo.name());

    if (newInfo.has_failover_timeout()) {
      info.set_failover_timeout(newInfo.failover_timeout());
    } else {
      info.clear_failover_timeout();
    }

    if (newInfo.checkpoint() != info.checkpoint()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.checkpoint to '"
                   << stringify(newInfo.checkpoint()) << "' for framework "
                   << id() << ". Check MESOS-703";
    }

    if (newInfo.has_hostname()) {
      info.set_hostname(newInfo.hostname());
    } else {
      info.clear_hostname();
    }

    if (newInfo.principal() != info.principal()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.principal to '"
                   << newInfo.principal() << "' for framework " << id()
                   << ". Check MESOS-703";
    }

    if (newInfo.has_webui_url()) {
      info.set_webui_url(newInfo.webui_url());
    } else {
      info.clear_webui_url();
    }

    if (newInfo.capabilities_size() > 0) {
      info.mutable_capabilities()->CopyFrom(newInfo.capabilities());
    } else {
      info.clear_capabilities();
    }
    capabilities = protobuf::framework::Capabilities(info.capabilities());

    if (newInfo.has_labels()) {
      info.mutable_labels()->CopyFrom(newInfo.labels());
    } else {
      info.clear_labels();
    }

    const std::set<std::string>& newRoles = roles;

    const std::set<std::string> removedRoles = [&]() {
      std::set<std::string> result = oldRoles;
      foreach (const std::string& role, newRoles) {
        result.erase(role);
      }
      return result;
    }();

    foreach (const std::string& role, removedRoles) {
      auto allocatedToRole = [&role](const Resource& resource) {
        return resource.allocation_info().role() == role;
      };

      // Stop tracking the framework under this role if there are
      // no longer any resources allocated to it.
      if (totalUsedResources.filter(allocatedToRole).empty()) {
        CHECK(totalOfferedResources.filter(allocatedToRole).empty());
        untrackUnderRole(role);
      }
    }

    const std::set<std::string> addedRoles = [&]() {
      std::set<std::string> result = newRoles;
      foreach (const std::string& role, oldRoles) {
        result.erase(role);
      }
      return result;
    }();

    foreach (const std::string& role, addedRoles) {
      // NOTE: It's possible that we're already tracking this framework
      // under the role because a framework can unsubscribe from a role
      // while it still has resources allocated to the role.
      if (!isTrackedUnderRole(role)) {
        trackUnderRole(role);
      }
    }
  }

  void updateConnection(const process::UPID& newPid)
  {
    // Cleanup the HTTP connnection if this is a downgrade from HTTP
    // to PID. Note that the connection may already be closed.
    if (http.isSome()) {
      closeHttpConnection();
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
    } else if (http.isSome()) {
      // Cleanup the old HTTP connection.
      // Note that master creates a new HTTP connection for every
      // subscribe request, so 'newHttp' should always be different
      // from 'http'.
      closeHttpConnection();
    }

    CHECK_NONE(http);

    http = newHttp;
  }

  // Closes the HTTP connection and stops the heartbeat.
  //
  // TODO(vinod): Currently `state` variable is set separately
  // from this method. We need to make sure these are in sync.
  void closeHttpConnection()
  {
    CHECK_SOME(http);

    if (connected() && !http->close()) {
      LOG(WARNING) << "Failed to close HTTP pipe for " << *this;
    }

    http = None();

    CHECK_SOME(heartbeater);

    terminate(heartbeater->get());
    wait(heartbeater->get());

    heartbeater = None();
  }

  void heartbeat()
  {
    CHECK_NONE(heartbeater);
    CHECK_SOME(http);

    // TODO(vinod): Make heartbeat interval configurable and include
    // this information in the SUBSCRIBED response.
    scheduler::Event event;
    event.set_type(scheduler::Event::HEARTBEAT);

    heartbeater =
      new Heartbeater<scheduler::Event, v1::scheduler::Event>(
          "framework " + stringify(info.id()),
          event,
          http.get(),
          DEFAULT_HEARTBEAT_INTERVAL);

    process::spawn(heartbeater->get());
  }

  bool active() const    { return state == ACTIVE; }
  bool connected() const { return state == ACTIVE || state == INACTIVE; }
  bool recovered() const { return state == RECOVERED; }

  bool isTrackedUnderRole(const std::string& role) const;
  void trackUnderRole(const std::string& role);
  void untrackUnderRole(const std::string& role);

  Master* const master;

  FrameworkInfo info;

  std::set<std::string> roles;

  protobuf::framework::Capabilities capabilities;

  // Frameworks can either be connected via HTTP or by message passing
  // (scheduler driver). At most one of `http` and `pid` will be set
  // according to the last connection made by the framework; neither
  // field will be set if the framework is in state `RECOVERED`.
  Option<HttpConnection> http;
  Option<process::UPID> pid;

  State state;

  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // Tasks that have not yet been launched because they are currently
  // being authorized.
  hashmap<TaskID, TaskInfo> pendingTasks;

  // TODO(bmahler): Make this private to enforce that `addTask()` and
  // `removeTask()` are used, and provide a const view into the tasks.
  hashmap<TaskID, Task*> tasks;

  // Tasks launched by this framework that have reached a terminal
  // state and have had all their updates acknowledged. We only keep a
  // fixed-size cache to avoid consuming too much memory. We use
  // boost::circular_buffer rather than BoundedHashMap because there
  // can be multiple completed tasks with the same task ID.
  boost::circular_buffer<process::Owned<Task>> completedTasks;

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

  // This is only set for HTTP frameworks.
  Option<process::Owned<Heartbeater<scheduler::Event, v1::scheduler::Event>>>
    heartbeater;

private:
  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            State state,
            const process::Time& time)
    : master(_master),
      info(_info),
      roles(protobuf::framework::getRoles(_info)),
      capabilities(_info.capabilities()),
      state(state),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(masterFlags.max_completed_tasks_per_framework),
      unreachableTasks(masterFlags.max_unreachable_tasks_per_framework)
  {
    foreach (const std::string& role, roles) {
      // NOTE: It's possible that we're already being tracked under the role
      // because a framework can unsubscribe from a role while it still has
      // resources allocated to the role.
      if (!isTrackedUnderRole(role)) {
        trackUnderRole(role);
      }
    }
  }

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
  Role() = delete;

  Role(const std::string& _role) : role(_role) {}

  void addFramework(Framework* framework)
  {
    frameworks[framework->id()] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id());
  }

  Resources allocatedResources() const
  {
    Resources resources;

    auto allocatedTo = [](const std::string& role) {
      return [role](const Resource& resource) {
        CHECK(resource.has_allocation_info());
        return resource.allocation_info().role() == role;
      };
    };

    foreachvalue (Framework* framework, frameworks) {
      resources += framework->totalUsedResources.filter(allocatedTo(role));
      resources += framework->totalOfferedResources.filter(allocatedTo(role));
    }

    return resources;
  }

  const std::string role;

  // NOTE: The dynamic role/quota relation is stored in and administrated
  // by the master. There is no direct representation of quota information
  // here to avoid duplication and to support that an operator can associate
  // quota with a role before the role is created. Such ordering of operator
  // requests prevents a race of premature unbounded allocation that setting
  // quota first is intended to contain.

  hashmap<FrameworkID, Framework*> frameworks;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
