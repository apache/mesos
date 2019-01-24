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

#ifndef __RESOURCE_PROVIDER_STORAGE_PROVIDER_PROCESS_HPP__
#define __RESOURCE_PROVIDER_STORAGE_PROVIDER_PROCESS_HPP__

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "csi/client.hpp"
#include "csi/rpc.hpp"
#include "csi/state.hpp"
#include "csi/utils.hpp"

#include "slave/container_daemon.hpp"

#include "status_update_manager/operation.hpp"

namespace mesos {
namespace internal {

// Storage local resource provider initially picks a random amount of time
// between `[0, b]`, where `b = DEFAULT_CSI_RETRY_BACKOFF_FACTOR`, to retry CSI
// calls related to `CREATE_DISK` or `DESTROY_DISK` operations. Subsequent
// retries are exponentially backed off based on this interval (e.g., 2nd retry
// uses a random value between `[0, b * 2^1]`, 3rd retry between `[0, b * 2^2]`,
// etc) up to a maximum of `DEFAULT_CSI_RETRY_INTERVAL_MAX`.
//
// TODO(chhsiao): Make the retry parameters configurable.
constexpr Duration DEFAULT_CSI_RETRY_BACKOFF_FACTOR = Seconds(10);
constexpr Duration DEFAULT_CSI_RETRY_INTERVAL_MAX = Minutes(10);


class StorageLocalResourceProviderProcess
  : public process::Process<StorageLocalResourceProviderProcess>
{
public:
  explicit StorageLocalResourceProviderProcess(
      const process::http::URL& _url,
      const std::string& _workDir,
      const ResourceProviderInfo& _info,
      const SlaveID& _slaveId,
      const Option<std::string>& _authToken,
      bool _strict);

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const resource_provider::Event& event);

  // Wrapper functions to make CSI calls and update RPC metrics. Made public for
  // testing purpose.
  //
  // The call is made asynchronously and thus no guarantee is provided on the
  // order in which calls are sent. Callers need to either ensure to not have
  // multiple conflicting calls in flight, or treat results idempotently.
  //
  // NOTE: We currently ensure this by 1) resource locking to forbid concurrent
  // calls on the same volume, and 2) no profile update while there are ongoing
  // `CREATE_DISK` or `DESTROY_DISK` operations.
  //
  // NOTE: Since this function uses `getService` to obtain the latest service
  // future, which depends on probe results, it is disabled for making probe
  // calls; `_call` should be used directly instead.
  template <
      csi::v0::RPC rpc,
      typename std::enable_if<rpc != csi::v0::PROBE, int>::type = 0>
  process::Future<csi::v0::Response<rpc>> call(
      const ContainerID& containerId,
      const csi::v0::Request<rpc>& request,
      const bool retry = false); // remains const in a mutable lambda.

  template <csi::v0::RPC rpc>
  process::Future<Try<csi::v0::Response<rpc>, process::grpc::StatusError>>
  _call(csi::v0::Client client, const csi::v0::Request<rpc>& request);

  template <csi::v0::RPC rpc>
  process::Future<process::ControlFlow<csi::v0::Response<rpc>>> __call(
      const Try<csi::v0::Response<rpc>, process::grpc::StatusError>& result,
      const Option<Duration>& backoff);

private:
  struct VolumeData
  {
    VolumeData(csi::state::VolumeState&& _state)
      : state(_state), sequence(new process::Sequence("volume-sequence")) {}

    csi::state::VolumeState state;

    // We run all CSI operations for the same volume on a sequence to
    // ensure that they are processed in a sequential order.
    process::Owned<process::Sequence> sequence;
  };

  void initialize() override;
  void fatal();

  // The recover functions are responsible to recover the state of the
  // resource provider and CSI volumes from checkpointed data.
  process::Future<Nothing> recover();
  process::Future<Nothing> recoverServices();
  process::Future<Nothing> recoverVolumes();
  process::Future<Nothing> recoverResourceProviderState();

  void doReliableRegistration();

  // The reconcile functions are responsible to reconcile the state of
  // the resource provider from the recovered state and other sources of
  // truth, such as CSI plugin responses or the status update manager.
  process::Future<Nothing> reconcileResourceProviderState();
  process::Future<Nothing> reconcileOperationStatuses();
  ResourceConversion reconcileResources(
      const Resources& checkpointed,
      const Resources& discovered);

  // Spawns a loop to watch for changes in the set of known profiles and update
  // the profile mapping and storage pools accordingly.
  void watchProfiles();

  // Update the profile mapping when the set of known profiles changes.
  // NOTE: This function never fails. If it fails to translate a new
  // profile, the resource provider will continue to operate with the
  // set of profiles it knows about.
  process::Future<Nothing> updateProfiles(const hashset<std::string>& profiles);

  // Reconcile the storage pools when the set of known profiles changes,
  // or a volume with an unknown profile is destroyed.
  process::Future<Nothing> reconcileStoragePools();

  // Returns true if the storage pools are allowed to be reconciled when
  // the operation is being applied.
  static bool allowsReconciliation(const Offer::Operation& operation);

  // Functions for received events.
  void subscribed(const resource_provider::Event::Subscribed& subscribed);
  void applyOperation(
      const resource_provider::Event::ApplyOperation& operation);
  void publishResources(
      const resource_provider::Event::PublishResources& publish);
  void acknowledgeOperationStatus(
      const resource_provider::Event::AcknowledgeOperationStatus& acknowledge);
  void reconcileOperations(
      const resource_provider::Event::ReconcileOperations& reconcile);

  // Returns a future of a CSI client that waits for the endpoint socket to
  // appear if necessary, then connects to the socket and check its readiness.
  process::Future<csi::v0::Client> waitService(const std::string& endpoint);

  // Returns a future of the latest CSI client for the specified plugin
  // container. If the container is not already running, this method will start
  // a new a new container daemon.
  process::Future<csi::v0::Client> getService(const ContainerID& containerId);

  // Lists all running plugin containers for this resource provider.
  // NOTE: This might return containers that are not actually running, e.g., if
  // they are being destroyed.
  process::Future<hashmap<ContainerID, Option<ContainerStatus>>>
  getContainers();

  // Waits for the specified plugin container to be terminated.
  process::Future<Nothing> waitContainer(const ContainerID& containerId);

  // Kills the specified plugin container.
  process::Future<Nothing> killContainer(const ContainerID& containerId);

  process::Future<Nothing> prepareIdentityService();

  // NOTE: This can only be called after `prepareIdentityService`.
  process::Future<Nothing> prepareControllerService();

  // NOTE: This can only be called after `prepareIdentityService` and
  // `prepareControllerService`.
  process::Future<Nothing> prepareNodeService();

  // Transitions the state of the specified volume from `CREATED` or
  // `CONTROLLER_PUBLISH` to `NODE_READY`.
  //
  // NOTE: This can only be called after `prepareControllerService` and
  // `prepareNodeService`.
  process::Future<Nothing> controllerPublish(const std::string& volumeId);

  // Transitions the state of the specified volume from `NODE_READY`,
  // `CONTROLLER_PUBLISH` or `CONTROLLER_UNPUBLISH` to `CREATED`.
  //
  // NOTE: This can only be called after `prepareControllerService` and
  // `prepareNodeService`.
  process::Future<Nothing> controllerUnpublish(const std::string& volumeId);

  // Transitions the state of the specified volume from `NODE_READY` or
  // `NODE_STAGE` to `VOL_READY`.
  //
  // NOTE: This can only be called after `prepareNodeService`.
  process::Future<Nothing> nodeStage(const std::string& volumeId);

  // Transitions the state of the specified volume from `VOL_READY`,
  // `NODE_STAGE` or `NODE_UNSTAGE` to `NODE_READY`.
  //
  // NOTE: This can only be called after `prepareNodeService`.
  process::Future<Nothing> nodeUnstage(const std::string& volumeId);

  // Transitions the state of the specified volume from `VOL_READY` or
  // `NODE_PUBLISH` to `PUBLISHED`.
  //
  // NOTE: This can only be called after `prepareNodeService`.
  process::Future<Nothing> nodePublish(const std::string& volumeId);

  // Transitions the state of the specified volume from `PUBLISHED`,
  // `NODE_PUBLISH` or `NODE_UNPUBLISH` to `VOL_READY`.
  //
  // NOTE: This can only be called after `prepareNodeService`.
  process::Future<Nothing> nodeUnpublish(const std::string& volumeId);

  // Returns a CSI volume ID.
  //
  // NOTE: This can only be called after `prepareControllerService`.
  process::Future<std::string> createVolume(
      const std::string& name,
      const Bytes& capacity,
      const DiskProfileAdaptor::ProfileInfo& profileInfo);

  // Returns true if the volume has been deprovisioned.
  //
  // NOTE: This can only be called after `prepareControllerService` and
  // `prepareNodeService` (since it may require `NodeUnpublishVolume`).
  process::Future<bool> deleteVolume(const std::string& volumeId);

  // Validates if a volume supports the capability of the specified profile.
  //
  // NOTE: This can only be called after `prepareIdentityService`.
  process::Future<Nothing> validateVolume(
      const std::string& volumeId,
      const Option<Labels>& metadata,
      const DiskProfileAdaptor::ProfileInfo& profileInfo);

  // NOTE: This can only be called after `prepareControllerService` and the
  // resource provider ID has been obtained.
  process::Future<Resources> listVolumes();

  // NOTE: This can only be called after `prepareControllerService` and the
  // resource provider ID has been obtained.
  process::Future<Resources> getCapacities();

  // Applies the operation. Speculative operations will be synchronously
  // applied. Do nothing if the operation is already in a terminal state.
  process::Future<Nothing> _applyOperation(const id::UUID& operationUuid);

  // Sends `OPERATION_DROPPED` without checkpointing the operation status.
  void dropOperation(
      const id::UUID& operationUuid,
      const Option<FrameworkID>& frameworkId,
      const Option<Offer::Operation>& operation,
      const std::string& message);

  process::Future<std::vector<ResourceConversion>> applyCreateDisk(
      const Resource& resource,
      const id::UUID& operationUuid,
      const Resource::DiskInfo::Source::Type& targetType,
      const Option<std::string>& targetProfile);
  process::Future<std::vector<ResourceConversion>> applyDestroyDisk(
      const Resource& resource);

  // Synchronously updates `totalResources` and the operation status and
  // then asks the status update manager to send status updates.
  Try<Nothing> updateOperationStatus(
      const id::UUID& operationUuid,
      const Try<std::vector<ResourceConversion>>& conversions);

  void garbageCollectOperationPath(const id::UUID& operationUuid);

  void checkpointResourceProviderState();
  void checkpointVolumeState(const std::string& volumeId);

  void sendResourceProviderStateUpdate();

  // NOTE: This is a callback for the status update manager and should
  // not be called directly.
  void sendOperationStatusUpdate(
      const UpdateOperationStatusMessage& update);

  enum State
  {
    RECOVERING,
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED,
    READY
  } state;

  const process::http::URL url;
  const std::string workDir;
  const std::string metaDir;
  const ContentType contentType;
  ResourceProviderInfo info;
  const std::string vendor;
  const SlaveID slaveId;
  const Option<std::string> authToken;
  const bool strict;

  std::shared_ptr<DiskProfileAdaptor> diskProfileAdaptor;

  std::string bootId;
  process::grpc::client::Runtime runtime;
  process::Owned<v1::resource_provider::Driver> driver;
  OperationStatusUpdateManager statusUpdateManager;

  // The mapping of known profiles fetched from the DiskProfileAdaptor.
  hashmap<std::string, DiskProfileAdaptor::ProfileInfo> profileInfos;

  hashmap<ContainerID, process::Owned<slave::ContainerDaemon>> daemons;
  hashmap<ContainerID, process::Owned<process::Promise<csi::v0::Client>>>
    services;

  Option<ContainerID> nodeContainerId;
  Option<ContainerID> controllerContainerId;
  Option<csi::v0::GetPluginInfoResponse> pluginInfo;
  csi::v0::PluginCapabilities pluginCapabilities;
  csi::v0::ControllerCapabilities controllerCapabilities;
  csi::v0::NodeCapabilities nodeCapabilities;
  Option<std::string> nodeId;

  // We maintain the following invariant: if one operation depends on
  // another, they cannot be in PENDING state at the same time, i.e.,
  // the result of the preceding operation must have been reflected in
  // the total resources.
  //
  // NOTE: We store the list of operations in a `LinkedHashMap` to
  // preserve the order we receive the operations in case we need it.
  LinkedHashMap<id::UUID, Operation> operations;
  Resources totalResources;
  id::UUID resourceVersion;
  hashmap<std::string, VolumeData> volumes;

  // If pending, it means that the storage pools are being reconciled, and all
  // incoming operations that disallow reconciliation will be dropped.
  process::Future<Nothing> reconciled;

  // We maintain a sequence to coordinate reconciliations of storage pools. It
  // keeps track of pending operations that disallow reconciliation, and ensures
  // that any reconciliation waits for these operations to finish.
  process::Sequence sequence;

  struct Metrics
  {
    explicit Metrics(const std::string& prefix);
    ~Metrics();

    // CSI plugin metrics.
    process::metrics::Counter csi_plugin_container_terminations;
    hashmap<csi::v0::RPC, process::metrics::PushGauge> csi_plugin_rpcs_pending;
    hashmap<csi::v0::RPC, process::metrics::Counter> csi_plugin_rpcs_successes;
    hashmap<csi::v0::RPC, process::metrics::Counter> csi_plugin_rpcs_errors;
    hashmap<csi::v0::RPC, process::metrics::Counter> csi_plugin_rpcs_cancelled;

    // Operation state metrics.
    hashmap<Offer::Operation::Type, process::metrics::PushGauge>
      operations_pending;
    hashmap<Offer::Operation::Type, process::metrics::Counter>
      operations_finished;
    hashmap<Offer::Operation::Type, process::metrics::Counter>
      operations_failed;
    hashmap<Offer::Operation::Type, process::metrics::Counter>
      operations_dropped;
  } metrics;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_STORAGE_PROVIDER_PROCESS_HPP__
