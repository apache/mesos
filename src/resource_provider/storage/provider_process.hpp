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

#include <memory>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

#include <stout/hashset.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "csi/metrics.hpp"
#include "csi/volume_manager.hpp"

#include "status_update_manager/operation.hpp"

namespace mesos {
namespace internal {

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

private:
  void initialize() override;
  void fatal();

  process::Future<Nothing> recover();

  void doReliableRegistration();

  // The reconcile functions are responsible to reconcile the state of
  // the resource provider from the recovered state and other sources of
  // truth, such as CSI plugin responses or the status update manager.
  process::Future<Nothing> reconcileResourceProviderState();
  process::Future<Nothing> reconcileOperationStatuses();
  ResourceConversion reconcileResources(
      const Resources& checkpointed,
      const Resources& discovered);

  process::Future<Resources> getRawVolumes();
  process::Future<Resources> getStoragePools();

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

  // Applies the operation. Speculative operations will be synchronously
  // applied. Do nothing if the operation is already in a terminal state.
  process::Future<Nothing> _applyOperation(const id::UUID& operationUuid);

  // Sends `OPERATION_DROPPED` status update. The operation status will be
  // checkpointed if `operation` is set.
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

  // Synchronously creates persistent volumes.
  Try<std::vector<ResourceConversion>> applyCreate(
      const Offer::Operation& operation) const;

  // Synchronously cleans up and destroys persistent volumes.
  Try<std::vector<ResourceConversion>> applyDestroy(
      const Offer::Operation& operation) const;

  // Synchronously updates `totalResources` and the operation status and
  // then asks the status update manager to send status updates.
  Try<Nothing> updateOperationStatus(
      const id::UUID& operationUuid,
      const Try<std::vector<ResourceConversion>>& conversions);

  void garbageCollectOperationPath(const id::UUID& operationUuid);

  void checkpointResourceProviderState();

  void sendResourceProviderStateUpdate();

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

  process::Owned<v1::resource_provider::Driver> driver;
  OperationStatusUpdateManager statusUpdateManager;

  // The mapping of known profiles fetched from the DiskProfileAdaptor.
  hashmap<std::string, DiskProfileAdaptor::ProfileInfo> profileInfos;

  process::Owned<csi::VolumeManager> volumeManager;

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

  // If pending, it means that the storage pools are being reconciled, and all
  // incoming operations that disallow reconciliation will be dropped.
  process::Future<Nothing> reconciled;

  // We maintain a sequence to coordinate reconciliations of storage pools. It
  // keeps track of pending operations that disallow reconciliation, and ensures
  // that any reconciliation waits for these operations to finish.
  process::Sequence sequence;

  struct Metrics : public csi::Metrics
  {
    explicit Metrics(const std::string& prefix);
    ~Metrics();

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
