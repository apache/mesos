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

#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <stdint.h>

#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/module/authenticatee.hpp>

#include <mesos/slave/containerizer.hpp>
#include <mesos/slave/qos_controller.hpp>
#include <mesos/slave/resource_estimator.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/http.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/limiter.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/shared.hpp>
#include <process/sequence.hpp>

#include <stout/boundedhashmap.hpp>
#include <stout/bytes.hpp>
#include <stout/circular_buffer.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/recordio.hpp>
#include <stout/uuid.hpp>

#include "common/heartbeater.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"

#include "files/files.hpp"

#include "internal/evolve.hpp"

#include "messages/messages.hpp"

#include "resource_provider/daemon.hpp"
#include "resource_provider/manager.hpp"

#include "slave/constants.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/http.hpp"
#include "slave/metrics.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "status_update_manager/operation.hpp"

// `REGISTERING` is used as an enum value, but it's actually defined as a
// constant in the Windows SDK.
#ifdef __WINDOWS__
#undef REGISTERING
#undef REGISTERED
#endif // __WINDOWS__

namespace mesos {

// Forward declarations.
class Authorizer;
class DiskProfileAdaptor;

namespace internal {
namespace slave {

// Some forward declarations.
class TaskStatusUpdateManager;
class Executor;
class Framework;

struct ResourceProvider;


class Slave : public ProtobufProcess<Slave>
{
public:
  Slave(const std::string& id,
        const Flags& flags,
        mesos::master::detector::MasterDetector* detector,
        Containerizer* containerizer,
        Files* files,
        GarbageCollector* gc,
        TaskStatusUpdateManager* taskStatusUpdateManager,
        mesos::slave::ResourceEstimator* resourceEstimator,
        mesos::slave::QoSController* qosController,
        mesos::SecretGenerator* secretGenerator,
        VolumeGidManager* volumeGidManager,
        PendingFutureTracker* futureTracker,
        process::Owned<CSIServer>&& csiServer,
#ifndef __WINDOWS__
        const Option<process::network::unix::Socket>& executorSocket,
#endif // __WINDOWS__
        const Option<Authorizer*>& authorizer);

  ~Slave() override;

  void shutdown(const process::UPID& from, const std::string& message);

  void registered(
      const process::UPID& from,
      const SlaveID& slaveId,
      const MasterSlaveConnection& connection);

  void reregistered(
      const process::UPID& from,
      const SlaveID& slaveId,
      const std::vector<ReconcileTasksMessage>& reconciliations,
      const MasterSlaveConnection& connection);

  void doReliableRegistration(Duration maxBackoff);

  // TODO(mzhu): Combine this with `runTask()' and replace all `runTask()'
  // mock with `run()` mock.
  void handleRunTaskMessage(
      const process::UPID& from,
      RunTaskMessage&& runTaskMessage);

  Option<Error> validateResourceLimitsAndIsolators(
      const std::vector<TaskInfo>& tasks);

  // Made 'virtual' for Slave mocking.
  virtual void runTask(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const process::UPID& pid,
      const TaskInfo& task,
      const std::vector<ResourceVersionUUID>& resourceVersionUuids,
      const Option<bool>& launchExecutor);

  void run(
      const FrameworkInfo& frameworkInfo,
      ExecutorInfo executorInfo,
      Option<TaskInfo> task,
      Option<TaskGroupInfo> taskGroup,
      const std::vector<ResourceVersionUUID>& resourceVersionUuids,
      const process::UPID& pid,
      const Option<bool>& launchExecutor,
      bool executorGeneratedForCommandTask);

  // Made 'virtual' for Slave mocking.
  //
  // This function returns a future so that we can encapsulate a task(group)
  // launch operation (from agent receiving the run message to the completion
  // of `_run()`) into a single future. This includes all the asynchronous
  // steps (currently two: unschedule GC and task authorization) prior to the
  // executor launch.
  virtual process::Future<Nothing> _run(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const Option<TaskInfo>& task,
      const Option<TaskGroupInfo>& taskGroup,
      const std::vector<ResourceVersionUUID>& resourceVersionUuids,
      const Option<bool>& launchExecutor);

  // Made 'virtual' for Slave mocking.
  virtual void __run(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const Option<TaskInfo>& task,
      const Option<TaskGroupInfo>& taskGroup,
      const std::vector<ResourceVersionUUID>& resourceVersionUuids,
      const Option<bool>& launchExecutor,
      bool executorGeneratedForCommandTask);

  // This is called when the resource limits of the container have
  // been updated for the given tasks and task groups. If the update is
  // successful, we flush the given tasks to the executor by sending
  // RunTaskMessages or `LAUNCH_GROUP` events.
  //
  // Made 'virtual' for Slave mocking.
  virtual void ___run(
      const process::Future<Nothing>& future,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      const std::vector<TaskInfo>& tasks,
      const std::vector<TaskGroupInfo>& taskGroups);

  // TODO(mzhu): Combine this with `runTaskGroup()' and replace all
  // `runTaskGroup()' mock with `run()` mock.
  void handleRunTaskGroupMessage(
      const process::UPID& from,
      RunTaskGroupMessage&& runTaskGroupMessage);

  // Made 'virtual' for Slave mocking.
  virtual void runTaskGroup(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const TaskGroupInfo& taskGroupInfo,
      const std::vector<ResourceVersionUUID>& resourceVersionUuids,
      const Option<bool>& launchExecutor);

  // Handler for the `KillTaskMessage`. Made 'virtual' for Slave mocking.
  virtual void killTask(
      const process::UPID& from,
      const KillTaskMessage& killTaskMessage);

  // Helper to kill a pending task, which may or may not be associated with a
  // valid `Executor` struct.
  void killPendingTask(
      const FrameworkID& frameworkId,
      Framework* framework,
      const TaskID& taskId);

  // Helper to kill a task belonging to a valid framework and executor. This
  // function should be used to kill tasks which are queued or launched, but
  // not tasks which are pending.
  void kill(
      const FrameworkID& frameworkId,
      Framework* framework,
      Executor* executor,
      const TaskID& taskId,
      const Option<KillPolicy>& killPolicy);

  // Made 'virtual' for Slave mocking.
  virtual void shutdownExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Shut down an executor. This is a two phase process. First, an
  // executor receives a shut down message (shut down phase), then
  // after a configurable timeout the slave actually forces a kill
  // (kill phase, via the isolator) if the executor has not
  // exited.
  //
  // Made 'virtual' for Slave mocking.
  virtual void _shutdownExecutor(Framework* framework, Executor* executor);

  void shutdownFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);

  void schedulerMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void updateFramework(
      const UpdateFrameworkMessage& message);

  void checkpointResourceState(const Resources& resources, bool changeTotal);

  void checkpointResourceState(
      std::vector<Resource> resources,
      bool changeTotal);

  void checkpointResourcesMessage(
      const std::vector<Resource>& resources);

  // Made 'virtual' for Slave mocking.
  virtual void applyOperation(const ApplyOperationMessage& message);

  // Reconciles pending operations with the master. This is necessary to handle
  // cases in which operations were dropped in transit, or in which an agent's
  // `UpdateSlaveMessage` was sent at the same time as an operation was en route
  // from the master to the agent.
  void reconcileOperations(const ReconcileOperationsMessage& message);

  void subscribe(
      StreamingHttpConnection<v1::executor::Event> http,
      const executor::Call::Subscribe& subscribe,
      Framework* framework,
      Executor* executor);

  void registerExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Called when an executor reregisters with a recovering slave.
  // 'tasks' : Unacknowledged tasks (i.e., tasks that the executor
  //           driver never received an ACK for.)
  // 'updates' : Unacknowledged updates.
  void reregisterExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::vector<TaskInfo>& tasks,
      const std::vector<StatusUpdate>& updates);

  void _reregisterExecutor(
      const process::Future<Nothing>& future,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  void executorMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void ping(const process::UPID& from, bool connected);

  // Handles the task status update.
  // NOTE: If 'pid' is a valid UPID an ACK is sent to this pid
  // after the update is successfully handled. If pid == UPID()
  // no ACK is sent. The latter is used by the slave to send
  // status updates it generated (e.g., TASK_LOST). If pid == None()
  // an ACK is sent to the corresponding HTTP based executor.
  // NOTE: StatusUpdate is passed by value because it is modified
  // to ensure source field is set.
  void statusUpdate(StatusUpdate update, const Option<process::UPID>& pid);

  // Called when the slave receives a `StatusUpdate` from an executor
  // and the slave needs to retrieve the container status for the
  // container associated with the executor.
  void _statusUpdate(
      StatusUpdate update,
      const Option<process::UPID>& pid,
      const ExecutorID& executorId,
      const Option<process::Future<ContainerStatus>>& containerStatus);

  // Continue handling the status update after optionally updating the
  // container's resources.
  void __statusUpdate(
      const Option<process::Future<Nothing>>& future,
      const StatusUpdate& update,
      const Option<process::UPID>& pid,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      bool checkpoint);

  // This is called when the task status update manager finishes
  // handling the update. If the handling is successful, an
  // acknowledgment is sent to the executor.
  void ___statusUpdate(
      const process::Future<Nothing>& future,
      const StatusUpdate& update,
      const Option<process::UPID>& pid);

  // This is called by task status update manager to forward a status
  // update to the master. Note that the latest state of the task is
  // added to the update before forwarding.
  void forward(StatusUpdate update);

  // This is called by the operation status update manager to forward operation
  // status updates to the master.
  void sendOperationStatusUpdate(const UpdateOperationStatusMessage& update);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void _statusUpdateAcknowledgement(
      const process::Future<bool>& future,
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const UUID& uuid);

  void operationStatusAcknowledgement(
      const process::UPID& from,
      const AcknowledgeOperationStatusMessage& acknowledgement);

  void drain(
      const process::UPID& from,
      DrainSlaveMessage&& drainSlaveMessage);

  void executorLaunched(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      const process::Future<Containerizer::LaunchResult>& future);

  // Made 'virtual' for Slave mocking.
  virtual void executorTerminated(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const process::Future<Option<
          mesos::slave::ContainerTermination>>& termination);

  // NOTE: Pulled these to public to make it visible for testing.
  // TODO(vinod): Make tests friends to this class instead.

  // Garbage collects the directories based on the current disk usage.
  // TODO(vinod): Instead of making this function public, we need to
  // mock both GarbageCollector (and pass it through slave's constructor)
  // and os calls.
  void _checkDiskUsage(const process::Future<double>& usage);

  // Garbage collect image layers based on the disk usage of image
  // store.
  void _checkImageDiskUsage(const process::Future<double>& usage);

  // Invoked whenever the detector detects a change in masters.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo>>& _master);

  enum State
  {
    RECOVERING,   // Slave is doing recovery.
    DISCONNECTED, // Slave is not connected to the master.
    RUNNING,      // Slave has (re-)registered.
    TERMINATING,  // Slave is shutting down.
  } state;

  // Describes information about agent recovery.
  struct RecoveryInfo
  {
    // Flag to indicate if recovery, including reconciling
    // (i.e., reconnect/kill) with executors is finished.
    process::Promise<Nothing> recovered;

    // Flag to indicate that HTTP based executors can
    // subscribe with the agent. We allow them to subscribe
    // after the agent recovers the containerizer.
    bool reconnect = false;
  } recoveryInfo;

  // TODO(benh): Clang requires members to be public in order to take
  // their address which we do in tests (for things like
  // FUTURE_DISPATCH).
// protected:
  void initialize() override;
  void finalize() override;
  void exited(const process::UPID& pid) override;

  // Generates a secret for executor authentication. Returns None if there is
  // no secret generator.
  process::Future<Option<Secret>> generateSecret(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  // `executorInfo` is a mutated executor info with some default fields and
  // resources. If an executor is launched for a task group, `taskInfo` would
  // not be set.
  void launchExecutor(
      const process::Future<Option<Secret>>& authorizationToken,
      const FrameworkID& frameworkId,
      const ExecutorInfo& executorInfo,
      const google::protobuf::Map<std::string, Value::Scalar>& executorLimits,
      const Option<TaskInfo>& taskInfo);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path,
                    const std::string& virtualPath);

  Nothing detachFile(const std::string& path);

  // TODO(qianzhang): This is a workaround to make the default executor task's
  // volume directory visible in MESOS UI. It handles two cases:
  //   1. The task has disk resources specified. In this case any disk resources
  //      specified for the task are mounted on the top level container since
  //      currently all resources of nested containers are merged in the top
  //      level executor container. To make sure the task can access any volumes
  //      specified in its disk resources from its sandbox, a workaround was
  //      introduced to the default executor in MESOS-7225, i.e., adding a
  //      `SANDBOX_PATH` volume with type `PARENT` to the corresponding nested
  //      container. This volume gets translated into a bind mount in the nested
  //      container's mount namespace, which is not visible in Mesos UI because
  //      it operates in the host namespace. See MESOS-8279 for details.
  //   2. The executor has disk resources specified and the task's ContainerInfo
  //      has a `SANDBOX_PATH` volume with type `PARENT` specified to share the
  //      executor's disk volume. Similar to the first case, this `SANDBOX_PATH`
  //      volume gets translated into a bind mount which is not visible in Mesos
  //      UI. See MESOS-8565 for details.
  //
  // To make the task's volume directory visible in Mesos UI, here we attach the
  // executor's volume directory to it so that it can be accessed via the /files
  // endpoint. So when users browse task's volume directory in Mesos UI, what
  // they actually browse is the executor's volume directory. Note when calling
  // `Files::attach()`, the third argument `authorized` is not specified because
  // it is already specified when we do the attach for the executor's sandbox
  // and it also applies to the executor's tasks. Note that for the second case
  // we can not do the attach when the task's ContainerInfo has a `SANDBOX_PATH`
  // volume with type `PARENT` but the executor has NO disk resources, because
  // in such case the attach will fail due to the executor's volume directory
  // not existing which will actually be created by the `volume/sandbox_path`
  // isolator when launching the nested container. But if the executor has disk
  // resources, then we will not have this issue since the executor's volume
  // directory will be created by the `filesystem/linux` isolator when launching
  // the executor before we do the attach.
  void attachTaskVolumeDirectory(
      const ExecutorInfo& executorInfo,
      const ContainerID& executorContainerId,
      const Task& task);

  // TODO(qianzhang): Remove the task's volume directory from the /files
  // endpoint. This is a workaround for MESOS-8279 and MESOS-8565.
  void detachTaskVolumeDirectories(
      const ExecutorInfo& executorInfo,
      const ContainerID& executorContainerId,
      const std::vector<Task>& tasks);

  // Triggers a re-detection of the master when the slave does
  // not receive a ping.
  void pingTimeout(process::Future<Option<MasterInfo>> future);

  // Made virtual for testing purpose.
  virtual void authenticate(Duration minTimeout, Duration maxTimeout);

  // Helper routines to lookup a framework/executor.
  Framework* getFramework(const FrameworkID& frameworkId) const;

  Executor* getExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId) const;

  Executor* getExecutor(const ContainerID& containerId) const;

  // Returns the ExecutorInfo associated with a TaskInfo. If the task has no
  // ExecutorInfo, then we generate an ExecutorInfo corresponding to the
  // command executor.
  ExecutorInfo getExecutorInfo(
      const FrameworkInfo& frameworkInfo,
      const TaskInfo& task) const;

  // Shuts down the executor if it did not register yet.
  void registerExecutorTimeout(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  // Cleans up all un-reregistered executors during recovery.
  void reregisterExecutorTimeout();

  // This function returns the max age of executor/slave directories allowed,
  // given a disk usage. This value could be used to tune gc.
  Duration age(double usage);

  // Checks the current disk usage and schedules for gc as necessary.
  void checkDiskUsage();

  // Checks the current container image disk usage and trigger image
  // gc if necessary.
  void checkImageDiskUsage();

  // Recovers the slave, task status update manager and isolator.
  process::Future<Nothing> recover(const Try<state::State>& state);

  // This is called after 'recover()'. If 'flags.reconnect' is
  // 'reconnect', the slave attempts to reconnect to any old live
  // executors. Otherwise, the slave attempts to shutdown/kill them.
  process::Future<Nothing> _recover();

  // This is a helper to call `recover()` on the volume gid manager.
  process::Future<Nothing> _recoverVolumeGidManager(bool rebooted);

  // This is a helper to call `recover()` on the task status update manager.
  process::Future<Option<state::SlaveState>> _recoverTaskStatusUpdates(
      const Option<state::SlaveState>& slaveState);

  // This is a helper to call `recover()` on the containerizer at the end of
  // `recover()` and before `__recover()`.
  // TODO(idownes): Remove this when we support defers to objects.
  process::Future<Nothing> _recoverContainerizer(
      const Option<state::SlaveState>& state);

  // This is called after `_recoverContainerizer()`. It will add all
  // checkpointed operations affecting agent default resources and call
  // `OperationStatusUpdateManager::recover()`.
  process::Future<Nothing> _recoverOperations(
      const Option<state::SlaveState>& state);

  // This is called after `OperationStatusUpdateManager::recover()`
  // completes.
  //
  // If necessary it will add any missing operation status updates
  // that couldn't be checkpointed before the agent failed over.
  process::Future<Nothing> __recoverOperations(
      const process::Future<OperationStatusUpdateManagerState>& state);

  // This is called when recovery finishes.
  // Made 'virtual' for Slave mocking.
  virtual void __recover(const process::Future<Nothing>& future);

  // Helper to recover a framework from the specified state.
  void recoverFramework(
      const state::FrameworkState& state,
      const hashset<ExecutorID>& executorsToRecheckpoint,
      const hashmap<ExecutorID, hashset<TaskID>>& tasksToRecheckpoint);

  // Removes and garbage collects the executor.
  void removeExecutor(Framework* framework, Executor* executor);

  // Removes and garbage collects the framework.
  // Made 'virtual' for Slave mocking.
  virtual void removeFramework(Framework* framework);

  // Schedules a 'path' for gc based on its modification time.
  process::Future<Nothing> garbageCollect(const std::string& path);

  // Called when the slave was signaled from the specified user.
  void signaled(int signal, int uid);

  // Made 'virtual' for Slave mocking.
  virtual void qosCorrections();

  // Made 'virtual' for Slave mocking.
  virtual void _qosCorrections(
      const process::Future<std::list<
          mesos::slave::QoSCorrection>>& correction);

  // Returns the resource usage information for all executors.
  virtual process::Future<ResourceUsage> usage();

  // Handle the second phase of shutting down an executor for those
  // executors that have not properly shutdown within a timeout.
  void shutdownExecutorTimeout(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

private:
  friend class Executor;
  friend class Framework;
  friend class Http;

  friend struct Metrics;

  Slave(const Slave&) = delete;
  Slave& operator=(const Slave&) = delete;

  void _authenticate(Duration currentMinTimeout, Duration currentMaxTimeout);

  // Process creation of persistent volumes (for CREATE) and/or deletion
  // of persistent volumes (for DESTROY) as a part of handling
  // checkpointed resources, and commit the checkpointed resources on
  // successful completion of all the operations.
  Try<Nothing> syncCheckpointedResources(
      const Resources& newCheckpointedResources);

  process::Future<bool> authorizeTask(
      const TaskInfo& task,
      const FrameworkInfo& frameworkInfo);

  process::Future<bool> authorizeSandboxAccess(
      const Option<process::http::authentication::Principal>& principal,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  void sendExecutorTerminatedStatusUpdate(
      const TaskID& taskId,
      const process::Future<Option<
          mesos::slave::ContainerTermination>>& termination,
      const FrameworkID& frameworkId,
      const Executor* executor);

  void sendExitedExecutorMessage(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Option<int>& status = None());

  // Forwards the current total of oversubscribed resources.
  void forwardOversubscribed();
  void _forwardOversubscribed(
      const process::Future<Resources>& oversubscribable);

  // Helper functions to generate `UpdateSlaveMessage` for either
  // just updates to resource provider-related information, or both
  // resource provider-related information and oversubscribed
  // resources.
  UpdateSlaveMessage generateResourceProviderUpdate() const;
  UpdateSlaveMessage generateUpdateSlaveMessage() const;

  void handleResourceProviderMessage(
      const process::Future<ResourceProviderMessage>& message);

  void addOperation(Operation* operation);

  // Transitions the operation, and recovers resource if the operation becomes
  // terminal.
  void updateOperation(
      Operation* operation,
      const UpdateOperationStatusMessage& update);

  // Update the `latest_status` of the operation if it is not terminal.
  void updateOperationLatestStatus(
      Operation* operation,
      const OperationStatus& status);

  void removeOperation(Operation* operation);

  process::Future<Nothing> markResourceProviderGone(
      const ResourceProviderID& resourceProviderId) const;

  Operation* getOperation(const UUID& uuid) const;

  void addResourceProvider(ResourceProvider* resourceProvider);
  ResourceProvider* getResourceProvider(const ResourceProviderID& id) const;

  void apply(Operation* operation);

  // Prepare all resources to be consumed by the specified container.
  process::Future<Nothing> publishResources(
      const ContainerID& containerId, const Resources& resources);

  // PullGauge methods.
  double _frameworks_active()
  {
    return static_cast<double>(frameworks.size());
  }

  double _uptime_secs()
  {
    return (process::Clock::now() - startTime).secs();
  }

  double _registered()
  {
    return master.isSome() ? 1 : 0;
  }

  double _tasks_staging();
  double _tasks_starting();
  double _tasks_running();
  double _tasks_killing();

  double _executors_registering();
  double _executors_running();
  double _executors_terminating();

  double _executor_directory_max_allowed_age_secs();

  double _resources_total(const std::string& name);
  double _resources_used(const std::string& name);
  double _resources_percent(const std::string& name);

  double _resources_revocable_total(const std::string& name);
  double _resources_revocable_used(const std::string& name);
  double _resources_revocable_percent(const std::string& name);

  // Checks whether the two `SlaveInfo` objects are considered
  // compatible based on the value of the `--configuration_policy`
  // flag.
  Try<Nothing> compatible(
      const SlaveInfo& previous,
      const SlaveInfo& current) const;

  void initializeResourceProviderManager(
      const Flags& flags,
      const SlaveID& slaveId);

  // This function is used to compute limits for executors before they
  // are launched as well as when updating running executors, so we must
  // accept both `TaskInfo` and `Task` types to handle both cases.
  google::protobuf::Map<std::string, Value::Scalar> computeExecutorLimits(
      const Resources& executorResources,
      const std::vector<TaskInfo>& taskInfos,
      const std::vector<Task*>& tasks = {}) const;

  protobuf::master::Capabilities requiredMasterCapabilities;

  const Flags flags;

  const Http http;

  SlaveInfo info;

  protobuf::slave::Capabilities capabilities;

  // Resources that are checkpointed by the slave.
  Resources checkpointedResources;

  // The current total resources of the agent, i.e., `info.resources()` with
  // checkpointed resources applied and resource provider resources.
  Resources totalResources;

  Option<process::UPID> master;

  hashmap<FrameworkID, Framework*> frameworks;

  // Note that these frameworks are "completed" only in that
  // they no longer have any active tasks or executors on this
  // particular agent.
  //
  // TODO(bmahler): Implement a more accurate framework lifecycle
  // in the agent code, ideally the master can inform the agent
  // when a framework is actually completed, and the agent can
  // perhaps store a cache of "idle" frameworks. See MESOS-7890.
  BoundedHashMap<FrameworkID, process::Owned<Framework>> completedFrameworks;

  mesos::master::detector::MasterDetector* detector;

  Containerizer* containerizer;

  Files* files;

  Metrics metrics;

  process::Time startTime;

  GarbageCollector* gc;

  TaskStatusUpdateManager* taskStatusUpdateManager;

  OperationStatusUpdateManager operationStatusUpdateManager;

  // Master detection future.
  process::Future<Option<MasterInfo>> detection;

  // Master's ping timeout value, updated on reregistration.
  Duration masterPingTimeout;

  // Timer for triggering re-detection when no ping is received from
  // the master.
  process::Timer pingTimer;

  // Timer for triggering agent (re)registration after detecting a new master.
  process::Timer agentRegistrationTimer;

  // Root meta directory containing checkpointed data.
  const std::string metaDir;

  // Indicates the number of errors ignored in "--no-strict" recovery mode.
  unsigned int recoveryErrors;

  Option<Credential> credential;

  // Authenticatee name as supplied via flags.
  std::string authenticateeName;

  Authenticatee* authenticatee;

  // Indicates if an authentication attempt is in progress.
  Option<process::Future<bool>> authenticating;

  // Indicates if the authentication is successful.
  bool authenticated;

  // Indicates if a new authentication attempt should be enforced.
  bool reauthenticate;

  // Maximum age of executor directories. Will be recomputed
  // periodically every flags.disk_watch_interval.
  Duration executorDirectoryMaxAllowedAge;

  mesos::slave::ResourceEstimator* resourceEstimator;

  mesos::slave::QoSController* qosController;

  std::shared_ptr<DiskProfileAdaptor> diskProfileAdaptor;

  mesos::SecretGenerator* secretGenerator;

  VolumeGidManager* volumeGidManager;

  PendingFutureTracker* futureTracker;

  process::Owned<CSIServer> csiServer;

#ifndef __WINDOWS__
  Option<process::network::unix::Socket> executorSocket;
#endif // __WINDOWS__

  Option<process::http::Server> executorSocketServer;

  const Option<Authorizer*> authorizer;

  // The most recent estimate of the total amount of oversubscribed
  // (allocated and oversubscribable) resources.
  Option<Resources> oversubscribedResources;

  process::Owned<ResourceProviderManager> resourceProviderManager;
  process::Owned<LocalResourceProviderDaemon> localResourceProviderDaemon;

  // Local resource providers known by the agent.
  hashmap<ResourceProviderID, ResourceProvider*> resourceProviders;

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

  // Keeps track of pending operations or terminal operations that
  // have unacknowledged status updates. These operations may affect
  // either agent default resources or resources offered by a resource
  // provider.
  hashmap<UUID, Operation*> operations;

  // Maps framework-supplied operation IDs to the operation's internal UUID.
  // This is used to satisfy some reconciliation requests which are forwarded
  // from the master to the agent.
  hashmap<std::pair<FrameworkID, OperationID>, UUID> operationIds;

  // Operations that are checkpointed by the agent.
  hashmap<UUID, Operation> checkpointedOperations;

  // If the agent is currently draining, contains the configuration used to
  // drain the agent. If NONE, the agent is not currently draining.
  Option<DrainConfig> drainConfig;

  // Time when this agent was last asked to drain. This field
  // is empty if the agent is not currently draining.
  Option<process::Time> estimatedDrainStartTime;

  // Check whether draining is finished and possibly remove
  // both in-memory and persisted drain configuration.
  void updateDrainStatus();
};


std::ostream& operator<<(std::ostream& stream, const Executor& executor);


// Information describing an executor.
class Executor
{
public:
  Executor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorInfo& info,
      const ContainerID& containerId,
      const std::string& directory,
      const Option<std::string>& user,
      bool checkpoint,
      bool isGeneratedForCommandTask);

  ~Executor();

  // Note that these tasks will also be tracked within `queuedTasks`.
  void enqueueTaskGroup(const TaskGroupInfo& taskGroup);

  void enqueueTask(const TaskInfo& task);
  Option<TaskInfo> dequeueTask(const TaskID& taskId);
  Task* addLaunchedTask(const TaskInfo& task);
  void completeTask(const TaskID& taskId);
  void checkpointExecutor();
  void checkpointTask(const TaskInfo& task);
  void checkpointTask(const Task& task);

  void recoverTask(const state::TaskState& state, bool recheckpointTask);

  void addPendingTaskStatus(const TaskStatus& status);
  void removePendingTaskStatus(const TaskStatus& status);

  Try<Nothing> updateTaskState(const TaskStatus& status);

  // Returns true if there are any queued/launched/terminated tasks.
  bool incompleteTasks();

  // Returns true if the agent ever sent any tasks to this executor.
  // More precisely, this function returns whether either:
  //
  //  (1) There are terminated/completed tasks with a
  //      SOURCE_EXECUTOR status.
  //
  //  (2) `launchedTasks` is not empty.
  //
  // If this function returns false and there are no queued tasks,
  // we probably (see TODO below) have killed or dropped all of its
  // initial tasks, in which case the agent will shut down the executor.
  //
  // TODO(mzhu): Note however, that since we look through the cache
  // of completed tasks, we can have false positives when a task
  // that was delivered to the executor has been evicted from the
  // completed task cache by tasks that have been killed by the
  // agent before delivery. This should be fixed.
  //
  // NOTE: This function checks whether any tasks has ever been sent,
  // this does not necessarily mean the executor has ever received
  // any tasks. Specifically, tasks in `launchedTasks` may be dropped
  // before received by the executor if the agent restarts.
  bool everSentTask() const;

  // Sends a message to the connected executor.
  template <typename Message>
  void send(const Message& message)
  {
    if (state == REGISTERING || state == TERMINATED) {
      LOG(WARNING) << "Attempting to send message to disconnected"
                   << " executor " << *this << " in state " << state;
    }

    if (http.isSome()) {
      if (!http->send(message)) {
        LOG(WARNING) << "Unable to send event to executor " << *this
                     << ": connection closed";
      }
    } else if (pid.isSome()) {
      slave->send(pid.get(), message);
    } else {
      LOG(WARNING) << "Unable to send event to executor " << *this
                   << ": unknown connection type";
    }
  }

  // Returns true if this executor is generated by Mesos for a command
  // task (either command executor for MesosContainerizer or docker
  // executor for DockerContainerizer).
  bool isGeneratedForCommandTask() const;

  // Closes the HTTP connection.
  void closeHttpConnection();

  // Returns the task group associated with the task.
  Option<TaskGroupInfo> getQueuedTaskGroup(const TaskID& taskId);

  Resources allocatedResources() const;

  enum State
  {
    REGISTERING,  // Executor is launched but not (re-)registered yet.
    RUNNING,      // Executor has (re-)registered.
    TERMINATING,  // Executor is being shutdown/killed.
    TERMINATED,   // Executor has terminated but there might be pending updates.
  } state;

  // We store the pointer to 'Slave' to get access to its methods
  // variables. One could imagine 'Executor' as being an inner class
  // of the 'Slave' class.
  Slave* slave;

  const ExecutorID id;
  const ExecutorInfo info;

  const FrameworkID frameworkId;

  const ContainerID containerId;

  const std::string directory;

  // The sandbox will be owned by this user and the executor will
  // run as this user. This can be set to None when --switch_user
  // is false or when compiled for Windows.
  const Option<std::string> user;

  const bool checkpoint;

  // An Executor can either be connected via HTTP or by libprocess
  // message passing. The following are the possible states:
  //
  // Agent State    Executor State       http       pid    Executor Type
  // -----------    --------------       ----      ----    -------------
  //  RECOVERING       REGISTERING       None     UPID()         Unknown
  //                   REGISTERING       None       Some      Libprocess
  //                   REGISTERING       None       None            HTTP
  //
  //           *       REGISTERING       None       None   Not known yet
  //           *                 *       None       Some      Libprocess
  //           *                 *       Some       None            HTTP
  Option<StreamingHttpConnection<v1::executor::Event>> http;
  process::Owned<ResponseHeartbeater<executor::Event, v1::executor::Event>>
    heartbeater;

  Option<process::UPID> pid;

  // Tasks can be found in one of the following four data structures:
  //
  // TODO(bmahler): Make these private to enforce that the task
  // lifecycle helper functions are not being bypassed, and provide
  // public views into them.

  // Not yet launched tasks. This also includes tasks from `queuedTaskGroups`.
  LinkedHashMap<TaskID, TaskInfo> queuedTasks;

  // Not yet launched task groups. This is needed for correctly sending
  // TASK_KILLED status updates for all tasks in the group if any of the
  // tasks were killed before the executor could register with the agent.
  std::vector<TaskGroupInfo> queuedTaskGroups;

  // Running.
  LinkedHashMap<TaskID, Task*> launchedTasks;

  // Terminated but pending updates.
  LinkedHashMap<TaskID, Task*> terminatedTasks;

  // Terminated and updates acked.
  // NOTE: We use a shared pointer for Task because clang doesn't like
  // stout's implementation of circular_buffer with Task (the Boost code
  // used internally by stout attempts to do some memset's which are unsafe).
  circular_buffer<std::shared_ptr<Task>> completedTasks;

  // When the slave initiates a destroy of the container, we expect a
  // termination to occur. The 'pendingTermation' indicates why the
  // slave initiated the destruction and will influence the
  // information sent in the status updates for any remaining
  // non-terminal tasks.
  Option<mesos::slave::ContainerTermination> pendingTermination;

  // Task status updates that are being processed by the agent.
  hashmap<TaskID, LinkedHashMap<id::UUID, TaskStatus>> pendingStatusUpdates;

private:
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  bool isGeneratedForCommandTask_;
};


// Information about a framework.
class Framework
{
public:
  Framework(
      Slave* slave,
      const Flags& slaveFlags,
      const FrameworkInfo& info,
      const Option<process::UPID>& pid);

  ~Framework();

  // Returns whether the framework is idle, where idle is
  // defined as having no activity:
  //   (1) The framework has no non-terminal tasks and executors.
  //   (2) All status updates have been acknowledged.
  //
  // TODO(bmahler): The framework should also not be considered
  // idle if there are unacknowledged updates for "pending" tasks.
  bool idle() const;

  void checkpointFramework() const;

  const FrameworkID id() const { return info.id(); }

  Try<Executor*> addExecutor(
    const ExecutorInfo& executorInfo,
    bool isGeneratedForCommandTask);

  Executor* getExecutor(const ExecutorID& executorId) const;
  Executor* getExecutor(const TaskID& taskId) const;

  void destroyExecutor(const ExecutorID& executorId);

  void recoverExecutor(
      const state::ExecutorState& state,
      bool recheckpointExecutor,
      const hashset<TaskID>& tasksToRecheckpoint);

  void addPendingTask(
      const ExecutorID& executorId,
      const TaskInfo& task);

  // Note that these tasks will also be tracked within `pendingTasks`.
  void addPendingTaskGroup(
      const ExecutorID& executorId,
      const TaskGroupInfo& taskGroup);

  bool hasTask(const TaskID& taskId) const;
  bool isPending(const TaskID& taskId) const;

  // Returns the task group associated with a pending task.
  Option<TaskGroupInfo> getTaskGroupForPendingTask(const TaskID& taskId);

  // Returns whether the pending task was removed.
  bool removePendingTask(const TaskID& taskId);

  Option<ExecutorID> getExecutorIdForPendingTask(const TaskID& taskId) const;

  Resources allocatedResources() const;

  enum State
  {
    RUNNING,      // First state of a newly created framework.
    TERMINATING,  // Framework is shutting down in the cluster.
  } state;

  // We store the pointer to 'Slave' to get access to its methods and
  // variables. One could imagine 'Framework' being an inner class of
  // the 'Slave' class.
  Slave* slave;

  FrameworkInfo info;

  protobuf::framework::Capabilities capabilities;

  // Frameworks using the scheduler driver will have a 'pid',
  // which allows us to send executor messages directly to the
  // driver. Frameworks using the HTTP API (in 0.24.0) will
  // not have a 'pid', in which case executor messages are
  // sent through the master.
  Option<process::UPID> pid;

  // Executors can be found in one of the following
  // data structures:
  //
  // TODO(bmahler): Make these private to enforce that
  // the executors lifecycle helper functions are not
  // being bypassed, and provide public views into them.

  // Executors with pending tasks.
  hashmap<ExecutorID, hashmap<TaskID, TaskInfo>> pendingTasks;

  // Sequences in this map are used to enforce the order of tasks launched on
  // each executor.
  //
  // Note on the lifecycle of the sequence: if the corresponding executor struct
  // has not been created, we tie the lifecycle of the sequence to the first
  // task in the sequence (which must have the `launch_executor` flag set to
  // true modulo MESOS-3870). If the task fails to launch before creating the
  // executor struct, we will delete the sequence. Once the executor struct is
  // created, we tie the lifecycle of the sequence to the executor struct.
  //
  // TODO(mzhu): Create the executor struct early and put the sequence in it.
  hashmap<ExecutorID, process::Sequence> taskLaunchSequences;

  // Pending task groups. This is needed for correctly sending
  // TASK_KILLED status updates for all tasks in the group if
  // any of the tasks are killed while pending.
  std::vector<TaskGroupInfo> pendingTaskGroups;

  // Current running executors.
  hashmap<ExecutorID, Executor*> executors;

  circular_buffer<process::Owned<Executor>> completedExecutors;

private:
  Framework(const Framework&) = delete;
  Framework& operator=(const Framework&) = delete;
};


struct ResourceProvider
{
  ResourceProvider(
      const ResourceProviderInfo& _info,
      const Resources& _totalResources,
      const Option<UUID>& _resourceVersion)
    : info(_info),
      totalResources(_totalResources),
      resourceVersion(_resourceVersion) {}

  void addOperation(Operation* operation);
  void removeOperation(Operation* operation);

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
  Option<UUID> resourceVersion;

  // Pending operations or terminal operations that have
  // unacknowledged status updates.
  hashmap<UUID, Operation*> operations;
};


/**
 * Returns a map of environment variables necessary in order to launch
 * an executor.
 *
 * @param executorInfo ExecutorInfo being launched.
 * @param directory Path to the sandbox directory.
 * @param slaveId SlaveID where this executor is being launched.
 * @param slavePid PID of the slave launching the executor.
 * @param checkpoint Whether or not the framework is checkpointing.
 * @param flags Flags used to launch the slave.
 *
 * @return Map of environment variables (name, value).
 */
std::map<std::string, std::string> executorEnvironment(
    const Flags& flags,
    const ExecutorInfo& executorInfo,
    const std::string& directory,
    const SlaveID& slaveId,
    const process::PID<Slave>& slavePid,
    const Option<Secret>& authenticationToken,
    bool checkpoint);


std::ostream& operator<<(std::ostream& stream, Executor::State state);
std::ostream& operator<<(std::ostream& stream, Framework::State state);
std::ostream& operator<<(std::ostream& stream, Slave::State state);

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HPP__
