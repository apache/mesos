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

#include "resource_provider/storage/provider.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <list>
#include <memory>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <mesos/http.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/push_gauge.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include <stout/os/realpath.hpp>

#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "csi/metrics.hpp"
#include "csi/paths.hpp"
#include "csi/service_manager.hpp"
#include "csi/volume_manager.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/constants.hpp"
#include "resource_provider/detector.hpp"
#include "resource_provider/state.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "status_update_manager/operation.hpp"

namespace http = process::http;

using std::accumulate;
using std::find;
using std::list;
using std::queue;
using std::shared_ptr;
using std::string;
using std::vector;

using process::await;
using process::collect;
using process::Continue;
using process::ControlFlow;
using process::defer;
using process::delay;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::Sequence;
using process::spawn;

using process::grpc::StatusError;

using process::grpc::client::Runtime;

using process::http::authentication::Principal;

using process::metrics::Counter;
using process::metrics::PushGauge;

using mesos::csi::ServiceManager;
using mesos::csi::VolumeInfo;
using mesos::csi::VolumeManager;

using mesos::internal::protobuf::convertLabelsToStringMap;
using mesos::internal::protobuf::convertStringMapToLabels;

using mesos::resource_provider::Call;
using mesos::resource_provider::DEFAULT_STORAGE_RECONCILIATION_INTERVAL;
using mesos::resource_provider::Event;
using mesos::resource_provider::ResourceProviderState;

using mesos::v1::resource_provider::Driver;

namespace mesos {
namespace internal {

// Returns true if the string is a valid Java identifier.
static bool isValidName(const string& s)
{
  if (s.empty()) {
    return false;
  }

  foreach (const char c, s) {
    if (!isalnum(c) && c != '_') {
      return false;
    }
  }

  return true;
}


// Returns true if the string is a valid Java package name.
static bool isValidType(const string& s)
{
  if (s.empty()) {
    return false;
  }

  foreach (const string& token, strings::split(s, ".")) {
    if (!isValidName(token)) {
      return false;
    }
  }

  return true;
}


// Returns the parent endpoint as a URL.
// TODO(jieyu): Consider using a more reliable way to get the agent v1
// operator API endpoint URL.
static inline http::URL extractParentEndpoint(const http::URL& url)
{
  http::URL parent = url;

  parent.path = Path(url.path).dirname();

  return parent;
}


static string getContainerPrefix(const ResourceProviderInfo& info)
{
  const Principal principal = LocalResourceProvider::principal(info);
  CHECK(principal.claims.contains("cid_prefix"));
  return principal.claims.at("cid_prefix");
}


static inline Resource createRawDiskResource(
    const ResourceProviderInfo& info,
    const Bytes& capacity,
    const Option<string>& profile,
    const Option<string>& vendor,
    const Option<string>& id = None(),
    const Option<Labels>& metadata = None())
{
  CHECK(info.has_id());
  CHECK(info.has_storage());

  Resource resource;
  resource.set_name("disk");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()
    ->set_value(static_cast<double>(capacity.bytes()) / Bytes::MEGABYTES);

  resource.mutable_provider_id()->CopyFrom(info.id()),
  resource.mutable_reservations()->CopyFrom(info.default_reservations());

  Resource::DiskInfo::Source* source =
    resource.mutable_disk()->mutable_source();

  source->set_type(Resource::DiskInfo::Source::RAW);

  if (profile.isSome()) {
    source->set_profile(profile.get());
  }

  if (vendor.isSome()) {
    source->set_vendor(vendor.get());
  }

  if (id.isSome()) {
    source->set_id(id.get());
  }

  if (metadata.isSome()) {
    source->mutable_metadata()->CopyFrom(metadata.get());
  }

  return resource;
}


class StorageLocalResourceProviderProcess
  : public Process<StorageLocalResourceProviderProcess>
{
public:
  explicit StorageLocalResourceProviderProcess(
      const http::URL& _url,
      const string& _workDir,
      const ResourceProviderInfo& _info,
      const SlaveID& _slaveId,
      const Option<string>& _authToken,
      bool _strict);

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const Event& event);

private:
  void initialize() override;
  void fatal();

  Future<Nothing> recover();

  void doReliableRegistration();

  // The reconcile functions are responsible to reconcile the state of
  // the resource provider from the recovered state and other sources of
  // truth, such as CSI plugin responses or the status update manager.
  Future<Nothing> reconcileResourceProviderState();
  Future<Nothing> reconcileOperationStatuses();

  // Query the plugin for its resources and update the providers
  // state. If `alwaysUpdate` is `true` an update will always be
  // sent, even if no changes are detected.
  Future<Nothing> reconcileResources(bool alwaysUpdate);

  ResourceConversion computeConversion(
      const Resources& checkpointed, const Resources& discovered) const;

  // Returns a list of resource conversions to updates volume contexts for
  // existing volumes, remove disappeared unconverted volumes, and add newly
  // appeared ones.
  Future<vector<ResourceConversion>> getExistingVolumes();

  // Returns a list of resource conversions to remove disappeared unconverted
  // storage pools and add newly appeared ones.
  Future<vector<ResourceConversion>> getStoragePools();

  // Spawns a loop to watch for changes in the set of known profiles and update
  // the profile mapping and storage pools accordingly.
  void watchProfiles();

  // Update the profile mapping when the set of known profiles changes.
  // NOTE: This function never fails. If it fails to translate a new
  // profile, the resource provider will continue to operate with the
  // set of profiles it knows about.
  Future<Nothing> updateProfiles(const hashset<string>& profiles);

  // Returns true if the storage pools are allowed to be reconciled when
  // the operation is being applied.
  static bool allowsReconciliation(const Offer::Operation& operation);

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void applyOperation(const Event::ApplyOperation& operation);
  void publishResources(const Event::PublishResources& publish);
  void acknowledgeOperationStatus(
      const Event::AcknowledgeOperationStatus& acknowledge);
  void reconcileOperations(const Event::ReconcileOperations& reconcile);

  // Periodically poll the provider for resource changes. The poll interval is
  // controlled by
  // `ResourceProviderInfo.Storage.reconciliation_interval_seconds`. When this
  // function is invoked it will perform the first poll after one reconciliation
  // interval.
  void watchResources();

  // Applies the operation. Speculative operations will be synchronously
  // applied. Do nothing if the operation is already in a terminal state.
  Future<Nothing> _applyOperation(const id::UUID& operationUuid);

  // Sends `OPERATION_DROPPED` status update. The operation status will be
  // checkpointed if `operation` is set.
  void dropOperation(
      const id::UUID& operationUuid,
      const Option<FrameworkID>& frameworkId,
      const Option<Offer::Operation>& operation,
      const string& message);

  Future<vector<ResourceConversion>> applyCreateDisk(
      const Resource& resource,
      const id::UUID& operationUuid,
      const Resource::DiskInfo::Source::Type& targetType,
      const Option<string>& targetProfile);

  Future<vector<ResourceConversion>> applyDestroyDisk(
      const Resource& resource);

  // Synchronously creates persistent volumes.
  Try<vector<ResourceConversion>> applyCreate(
      const Offer::Operation& operation) const;

  // Synchronously cleans up and destroys persistent volumes.
  Try<vector<ResourceConversion>> applyDestroy(
      const Offer::Operation& operation) const;

  // Synchronously updates `totalResources` and the operation status and
  // then asks the status update manager to send status updates.
  Try<Nothing> updateOperationStatus(
      const id::UUID& operationUuid,
      const Try<vector<ResourceConversion>>& conversions);

  void garbageCollectOperationPath(const id::UUID& operationUuid);

  void checkpointResourceProviderState();

  void sendResourceProviderStateUpdate();

  void sendOperationStatusUpdate(const UpdateOperationStatusMessage& update);

  enum State
  {
    RECOVERING,
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED,
    READY
  } state;

  const http::URL url;
  const string workDir;
  const string metaDir;
  const ContentType contentType;
  ResourceProviderInfo info;
  const string vendor;
  const SlaveID slaveId;
  const Option<string> authToken;
  const bool strict;

  const Duration reconciliationInterval;

  shared_ptr<DiskProfileAdaptor> diskProfileAdaptor;

  Owned<Driver> driver;
  OperationStatusUpdateManager statusUpdateManager;

  // The mapping of known profiles fetched from the DiskProfileAdaptor.
  hashmap<string, DiskProfileAdaptor::ProfileInfo> profileInfos;

  Runtime runtime;

  // NOTE: `metrics` must be destructed after `volumeManager` and
  // `serviceManager` since they hold a pointer to it.
  struct Metrics : public csi::Metrics
  {
    explicit Metrics(const string& prefix);
    ~Metrics();

    hashmap<Offer::Operation::Type, PushGauge> operations_pending;
    hashmap<Offer::Operation::Type, Counter> operations_finished;
    hashmap<Offer::Operation::Type, Counter> operations_failed;
    hashmap<Offer::Operation::Type, Counter> operations_dropped;
  } metrics;

  // NOTE: `serviceManager` must be destructed after `volumeManager` since the
  // latter holds a pointer of the former.
  Owned<ServiceManager> serviceManager;
  Owned<VolumeManager> volumeManager;

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
  Future<Nothing> reconciled;

  // We maintain a sequence to coordinate reconciliations of storage pools. It
  // keeps track of pending operations that disallow reconciliation, and ensures
  // that any reconciliation waits for these operations to finish.
  Sequence sequence;
};


StorageLocalResourceProviderProcess::StorageLocalResourceProviderProcess(
    const http::URL& _url,
    const string& _workDir,
    const ResourceProviderInfo& _info,
    const SlaveID& _slaveId,
    const Option<string>& _authToken,
    bool _strict)
  : ProcessBase(process::ID::generate("storage-local-resource-provider")),
    state(RECOVERING),
    url(_url),
    workDir(_workDir),
    metaDir(slave::paths::getMetaRootDir(_workDir)),
    contentType(ContentType::PROTOBUF),
    info(_info),
    vendor(
        info.storage().plugin().type() + "." + info.storage().plugin().name()),
    slaveId(_slaveId),
    authToken(_authToken),
    strict(_strict),
    reconciliationInterval(
        _info.storage().has_reconciliation_interval_seconds()
          ? Seconds(info.storage().reconciliation_interval_seconds())
          : DEFAULT_STORAGE_RECONCILIATION_INTERVAL),
    metrics("resource_providers/" + info.type() + "." + info.name() + "/"),
    resourceVersion(id::UUID::random()),
    sequence("storage-local-resource-provider-sequence")
{
  diskProfileAdaptor = DiskProfileAdaptor::getAdaptor();
  CHECK_NOTNULL(diskProfileAdaptor.get());
}


void StorageLocalResourceProviderProcess::connected()
{
  CHECK(state == DISCONNECTED || state == READY)
      << "Unexpected state: " << state;

  LOG(INFO) << "Connected to resource provider manager";

  state = CONNECTED;

  doReliableRegistration();
}


void StorageLocalResourceProviderProcess::disconnected()
{
  CHECK(state == CONNECTED || state == SUBSCRIBED || state == READY);

  LOG(INFO) << "Disconnected from resource provider manager";

  state = DISCONNECTED;

  statusUpdateManager.pause();
}


void StorageLocalResourceProviderProcess::received(const Event& event)
{
  LOG(INFO) << "Received " << event.type() << " event";

  switch (event.type()) {
    case Event::SUBSCRIBED: {
      CHECK(event.has_subscribed());
      subscribed(event.subscribed());
      break;
    }
    case Event::APPLY_OPERATION: {
      CHECK(event.has_apply_operation());
      applyOperation(event.apply_operation());
      break;
    }
    case Event::PUBLISH_RESOURCES: {
      CHECK(event.has_publish_resources());
      publishResources(event.publish_resources());
      break;
    }
    case Event::ACKNOWLEDGE_OPERATION_STATUS: {
      CHECK(event.has_acknowledge_operation_status());
      acknowledgeOperationStatus(event.acknowledge_operation_status());
      break;
    }
    case Event::RECONCILE_OPERATIONS: {
      CHECK(event.has_reconcile_operations());
      reconcileOperations(event.reconcile_operations());
      break;
    }
    case Event::TEARDOWN: {
      // TODO(bbannier): Clean up state after teardown.
      break;
    }
    case Event::UNKNOWN: {
      LOG(WARNING) << "Received an UNKNOWN event and ignored";
      break;
    }
  }
}


void StorageLocalResourceProviderProcess::initialize()
{
  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Failed to recover resource provider with type '" << info.type()
      << "' and name '" << info.name() << "': " << message;
    fatal();
  };

  // NOTE: Most resource provider events rely on the plugins being
  // prepared. To avoid race conditions, we connect to the agent after
  // preparing the plugins.
  recover()
    .onFailed(defer(self(), std::bind(die, lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, "future discarded")));
}


void StorageLocalResourceProviderProcess::fatal()
{
  // Force the disconnection early.
  driver.reset();

  process::terminate(self());
}


Future<Nothing> StorageLocalResourceProviderProcess::recover()
{
  CHECK_EQ(RECOVERING, state);

  serviceManager.reset(new ServiceManager(
      slaveId,
      extractParentEndpoint(url),
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin(),
      {csi::CONTROLLER_SERVICE, csi::NODE_SERVICE},
      getContainerPrefix(info),
      authToken,
      runtime,
      &metrics));

  return serviceManager->recover()
    .then(defer(self(), [=] {
      return serviceManager->getApiVersion();
    }))
    .then(defer(self(), [=](const string& apiVersion) -> Future<Nothing> {
      Try<Owned<VolumeManager>> volumeManager_ = VolumeManager::create(
          slave::paths::getCsiRootDir(workDir),
          info.storage().plugin(),
          {csi::CONTROLLER_SERVICE, csi::NODE_SERVICE},
          apiVersion,
          runtime,
          serviceManager.get(),
          &metrics);

      if (volumeManager_.isError()) {
        return Failure(
            "Failed to create CSI volume manager for resource provider with "
            "type '" + info.type() + "' and name '" + info.name() + "': " +
            volumeManager_.error());
      }

      volumeManager = std::move(volumeManager_.get());

      return volumeManager->recover();
    }))
    .then(defer(self(), [=]() -> Future<Nothing> {
      // Recover the resource provider ID and state from the latest symlink. If
      // the symlink does not exist, this is a new resource provider, and the
      // total resources will be empty, which is fine since new resources will
      // be added during reconciliation.
      Result<string> realpath =
        os::realpath(slave::paths::getLatestResourceProviderPath(
            metaDir, slaveId, info.type(), info.name()));

      if (realpath.isError()) {
        return Failure(
            "Failed to read the latest symlink for resource provider with type "
            "'" + info.type() + "' and name '" + info.name() + "': " +
            realpath.error());
      }

      if (realpath.isSome()) {
        info.mutable_id()->set_value(Path(realpath.get()).basename());

        const string statePath = slave::paths::getResourceProviderStatePath(
            metaDir, slaveId, info.type(), info.name(), info.id());

        if (os::exists(statePath)) {
          Result<ResourceProviderState> resourceProviderState =
            slave::state::read<ResourceProviderState>(statePath);

          if (resourceProviderState.isError()) {
            return Failure(
                "Failed to read resource provider state from '" + statePath +
                "': " + resourceProviderState.error());
          }

          if (resourceProviderState.isSome()) {
            foreach (const Operation& operation,
                     resourceProviderState->operations()) {
              Try<id::UUID> uuid =
                id::UUID::fromBytes(operation.uuid().value());

              operations[CHECK_NOTERROR(uuid)] = operation;
            }

            totalResources = resourceProviderState->resources();

            const ResourceProviderState::Storage& storage =
              resourceProviderState->storage();

            using ProfileEntry = google::protobuf::
              MapPair<string, ResourceProviderState::Storage::ProfileInfo>;

            foreach (const ProfileEntry& entry, storage.profiles()) {
              profileInfos.put(
                  entry.first,
                  {entry.second.capability(), entry.second.parameters()});
            }

            // We only checkpoint profiles associated with storage pools (i.e.,
            // resources without IDs) in `checkpointResourceProviderState` as
            // only these profiles might be used by pending operations, so we
            // validate here that all such profiles exist.
            foreach (const Resource& resource, totalResources) {
              if (!resource.disk().source().has_id() &&
                  resource.disk().source().has_profile() &&
                  !profileInfos.contains(resource.disk().source().profile())) {
                return Failure(
                    "Cannot recover profile for storage pool '" +
                    stringify(resource) + "' from '" + statePath + "'");
              }
            }
          }
        }
      }

      LOG(INFO)
        << "Recovered resources '" << totalResources << "' and "
        << operations.size() << " operations for resource provider with type '"
        << info.type() << "' and name '" << info.name() << "'";

      state = DISCONNECTED;

      statusUpdateManager.pause();

      driver.reset(new Driver(
          Owned<EndpointDetector>(new ConstantEndpointDetector(url)),
          contentType,
          defer(self(), &Self::connected),
          defer(self(), &Self::disconnected),
          defer(self(), [this](queue<v1::resource_provider::Event> events) {
            while(!events.empty()) {
              const v1::resource_provider::Event& event = events.front();
              received(devolve(event));
              events.pop();
            }
          }),
          authToken));

      driver->start();

      return Nothing();
    }));
}


void StorageLocalResourceProviderProcess::doReliableRegistration()
{
  if (state == DISCONNECTED || state == SUBSCRIBED || state == READY) {
    return;
  }

  CHECK_EQ(CONNECTED, state);

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_resource_provider_info()->CopyFrom(info);

  auto err = [](const ResourceProviderInfo& info, const string& message) {
    LOG(ERROR)
      << "Failed to subscribe resource provider with type '" << info.type()
      << "' and name '" << info.name() << "': " << message;
  };

  driver->send(evolve(call))
    .onFailed(std::bind(err, info, lambda::_1))
    .onDiscarded(std::bind(err, info, "future discarded"));

  // TODO(chhsiao): Consider doing an exponential backoff.
  delay(Seconds(1), self(), &Self::doReliableRegistration);
}


Future<Nothing>
StorageLocalResourceProviderProcess::reconcileResourceProviderState()
{
  return reconcileOperationStatuses()
    .then(defer(self(), &Self::reconcileResources, true))
    .then(defer(self(), [this] {
      statusUpdateManager.resume();

      switch (state) {
        case RECOVERING:
        case DISCONNECTED:
        case CONNECTED:
        case SUBSCRIBED: {
          LOG(INFO) << "Resource provider " << info.id()
                    << " is in READY state";

          state = READY;
        }
        case READY:
          break;
      }

      return Nothing();
    }));
}


void StorageLocalResourceProviderProcess::watchResources()
{
  // A specified reconciliation interval of zero
  // denotes disabled periodic reconciliations.
  if (reconciliationInterval == Seconds(0)) {
    return;
  }

  CHECK(info.has_id());

  loop(
      self(),
      std::bind(&process::after, reconciliationInterval),
      [this](const Nothing&) {
        // Poll resource provider state in `sequence` to
        // prevent concurrent non-reconcilable operations.
        reconciled = sequence.add(std::function<Future<Nothing>()>(
            defer(self(), &Self::reconcileResources, false)));

        return reconciled.then(
            [](const Nothing&) -> ControlFlow<Nothing> { return Continue(); });
      });
}


Future<Nothing> StorageLocalResourceProviderProcess::reconcileResources(
    bool alwaysUpdate)
{
  LOG(INFO) << "Reconciling storage pools and volumes";

  CHECK_PENDING(reconciled);

  return collect<vector<ResourceConversion>>(
             {getExistingVolumes(), getStoragePools()})
    .then(defer(
        self(),
        [alwaysUpdate, this]
        (const vector<vector<ResourceConversion>>& collected) {
          Resources result = totalResources;
          foreach (const vector<ResourceConversion>& conversions, collected) {
            result = CHECK_NOTERROR(result.apply(conversions));
          }

          bool shouldSendUpdate = alwaysUpdate;

          if (result != totalResources) {
            LOG(INFO) << "Removing '" << (totalResources - result)
                      << "' and adding '" << (result - totalResources)
                      << "' to the total resources";

            // Update the resource version since the total resources changed.
            totalResources = result;

            checkpointResourceProviderState();

            shouldSendUpdate = true;
          }

          if (shouldSendUpdate) {
            sendResourceProviderStateUpdate();
          }

          return Nothing();
        }));
}


Future<Nothing>
StorageLocalResourceProviderProcess::reconcileOperationStatuses()
{
  CHECK(info.has_id());

  const string resourceProviderDir = slave::paths::getResourceProviderPath(
      metaDir, slaveId, info.type(), info.name(), info.id());

  statusUpdateManager.initialize(
      defer(self(), &Self::sendOperationStatusUpdate, lambda::_1),
      std::bind(
          &slave::paths::getOperationUpdatesPath,
          resourceProviderDir,
          lambda::_1));

  Try<list<string>> operationPaths = slave::paths::getOperationPaths(
      slave::paths::getResourceProviderPath(
          metaDir, slaveId, info.type(), info.name(), info.id()));

  if (operationPaths.isError()) {
    return Failure(
        "Failed to find operations for resource provider " +
        stringify(info.id()) + ": " + operationPaths.error());
  }

  list<id::UUID> operationUuids;
  foreach (const string& path, operationPaths.get()) {
    Try<id::UUID> uuid =
      slave::paths::parseOperationPath(resourceProviderDir, path);

    if (uuid.isError()) {
      return Failure(
          "Failed to parse operation path '" + path + "': " +
          uuid.error());
    }

    // NOTE: This could happen if we failed to remove the operation path before.
    if (!operations.contains(uuid.get())) {
      LOG(WARNING)
        << "Ignoring unknown operation (uuid: " << uuid.get()
        << ") for resource provider " << info.id();

      garbageCollectOperationPath(uuid.get());
      continue;
    }

    operationUuids.emplace_back(std::move(uuid.get()));
  }

  return statusUpdateManager.recover(operationUuids, strict)
    .then(defer(self(), [=](
        const OperationStatusUpdateManagerState& statusUpdateManagerState)
        -> Future<Nothing> {
      using StreamState =
        typename OperationStatusUpdateManagerState::StreamState;

      // Clean up the operations that are completed.
      vector<id::UUID> completedOperations;
      foreachpair (const id::UUID& uuid,
                   const Option<StreamState>& stream,
                   statusUpdateManagerState.streams) {
        if (stream.isSome() && stream->terminated) {
          operations.erase(uuid);
          completedOperations.push_back(uuid);
        }
      }

      // Garbage collect the operation streams after checkpointing.
      checkpointResourceProviderState();
      foreach (const id::UUID& uuid, completedOperations) {
        garbageCollectOperationPath(uuid);
      }

      // Send updates for all missing statuses.
      foreachpair (const id::UUID& uuid,
                   const Operation& operation,
                   operations) {
        if (operation.latest_status().state() == OPERATION_PENDING) {
          continue;
        }

        const int numStatuses =
          statusUpdateManagerState.streams.contains(uuid) &&
          statusUpdateManagerState.streams.at(uuid).isSome()
            ? statusUpdateManagerState.streams.at(uuid)->updates.size() : 0;

        for (int i = numStatuses; i < operation.statuses().size(); i++) {
          UpdateOperationStatusMessage update =
            protobuf::createUpdateOperationStatusMessage(
                protobuf::createUUID(uuid),
                operation.statuses(i),
                None(),
                operation.has_framework_id()
                  ? operation.framework_id() : Option<FrameworkID>::none(),
                slaveId);

          auto die = [=](const string& message) {
            LOG(ERROR)
              << "Failed to update status of operation (uuid: " << uuid << "): "
              << message;
            fatal();
          };

          statusUpdateManager.update(std::move(update))
            .onFailed(defer(self(), std::bind(die, lambda::_1)))
            .onDiscarded(defer(self(), std::bind(die, "future discarded")));
        }
      }

      // We replay all pending operations here, so that if a volume is
      // created or deleted before the last failover, the result will be
      // reflected in the total resources before reconciliation.
      vector<Future<Nothing>> futures;

      foreachpair (const id::UUID& uuid,
                   const Operation& operation,
                   operations) {
        switch (operation.latest_status().state()) {
          case OPERATION_PENDING:
            ++metrics.operations_pending.at(operation.info().type());
            break;
          case OPERATION_FINISHED:
            ++metrics.operations_finished.at(operation.info().type());
            break;
          case OPERATION_FAILED:
            ++metrics.operations_failed.at(operation.info().type());
            break;
          case OPERATION_DROPPED:
            ++metrics.operations_dropped.at(operation.info().type());
            break;
          case OPERATION_UNSUPPORTED:
          case OPERATION_ERROR:
          case OPERATION_UNREACHABLE:
          case OPERATION_GONE_BY_OPERATOR:
          case OPERATION_RECOVERING:
          case OPERATION_UNKNOWN:
            UNREACHABLE();
        }

        if (protobuf::isTerminalState(operation.latest_status().state())) {
          continue;
        }

        auto err = [](const id::UUID& uuid, const string& message) {
          LOG(ERROR)
            << "Failed to apply operation (uuid: " << uuid << "): "
            << message;
        };

        futures.push_back(_applyOperation(uuid)
          .onFailed(std::bind(err, uuid, lambda::_1))
          .onDiscarded(std::bind(err, uuid, "future discarded")));
      }

      // We await the futures instead of collect them because it is OK
      // for operations to fail.
      return await(futures).then([] { return Nothing(); });
    }));
}


bool StorageLocalResourceProviderProcess::allowsReconciliation(
    const Offer::Operation& operation)
{
  switch (operation.type()) {
    case Offer::Operation::RESERVE:
    case Offer::Operation::UNRESERVE: {
      Resources consumedStoragePools =
        CHECK_NOTERROR(protobuf::getConsumedResources(operation))
          .filter([](const Resource& r) {
            return r.disk().source().has_profile() &&
              r.disk().source().type() == Resource::DiskInfo::Source::RAW;
          });

      return consumedStoragePools.empty();
    }
    case Offer::Operation::CREATE:
    case Offer::Operation::DESTROY: {
      return true;
    }
    case Offer::Operation::CREATE_DISK:
    case Offer::Operation::DESTROY_DISK: {
      return false;
    }
    case Offer::Operation::GROW_VOLUME:
    case Offer::Operation::SHRINK_VOLUME: {
      // TODO(chhsiao): These operations are currently not supported for
      // resource providers, and should have been validated by the master.
      UNREACHABLE();
    }
    case Offer::Operation::UNKNOWN:
    case Offer::Operation::LAUNCH:
    case Offer::Operation::LAUNCH_GROUP: {
      UNREACHABLE();
    }
  }

  UNREACHABLE();
}


ResourceConversion StorageLocalResourceProviderProcess::computeConversion(
    const Resources& checkpointed, const Resources& discovered) const
{
  // NOTE: If a resource in the checkpointed resources is missing in the
  // discovered resources, we will still keep it if it is converted by
  // an operation before (i.e., has extra info other than the default
  // reservations). The reason is that we want to maintain a consistent
  // view with frameworks, and do not want to lose any data on
  // persistent volumes due to some temporarily CSI plugin faults. Other
  // missing resources that are "unconverted" by any framework will be
  // removed. Then, any newly discovered resource will be added.
  Resources toRemove;
  Resources toAdd = discovered;

  foreach (const Resource& resource, checkpointed) {
    Resources unconverted = createRawDiskResource(
        info,
        Bytes(resource.scalar().value() * Bytes::MEGABYTES),
        resource.disk().source().has_profile()
          ? resource.disk().source().profile() : Option<string>::none(),
        resource.disk().source().has_vendor()
          ? resource.disk().source().vendor() : Option<string>::none(),
        resource.disk().source().has_id()
          ? resource.disk().source().id() : Option<string>::none(),
        resource.disk().source().has_metadata()
          ? resource.disk().source().metadata() : Option<Labels>::none());

    if (toAdd.contains(unconverted)) {
      // If the remaining of the discovered resources contain the
      // "unconverted" version of a checkpointed resource, this is not a
      // new resource.
      toAdd -= unconverted;
    } else if (unconverted == resource) {
      // If the remaining of the discovered resources does not contain
      // the "unconverted" version of the checkpointed resource, the
      // resource is missing. However, if it remains unconverted in the
      // checkpoint, we can safely remove it from the total resources.
      toRemove += unconverted;
    } else {
      LOG(WARNING)
        << "Missing converted resource '" << resource
        << "'. This might cause further operations to fail.";
    }
  }

  return ResourceConversion(std::move(toRemove), std::move(toAdd));
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::getExistingVolumes()
{
  CHECK(info.has_id());

  return volumeManager->listVolumes()
    .then(defer(self(), [=](const vector<VolumeInfo>& volumeInfos) {
      // If a volume is duplicated or a volume context has been changed by a
      // non-conforming CSI plugin, we need to construct a resources conversion
      // to remove the duplicate and update the metadata, so we maintain the
      // resources to be removed and those to be added here.
      Resources toRemove;
      Resources toAdd;

      // Since we only support "exclusive" (MOUNT or BLOCK) disks, there should
      // be only one checkpointed resource for each volume ID.
      hashmap<string, Resource> checkpointedMap;
      foreach (const Resource& resource, totalResources) {
        if (resource.disk().source().has_id()) {
          // If the checkpointed resources contain duplicated volumes because of
          // a non-conforming CSI plugin, remove the duplicate.
          if (checkpointedMap.contains(resource.disk().source().id())) {
            LOG(WARNING) << "Removing duplicated volume '" << resource
                         << "' from the total resources";

            toRemove += resource;
          } else {
            checkpointedMap.put(resource.disk().source().id(), resource);
          }
        }
      }

      // The "discovered" resources consist of RAW disk resources, one for each
      // volume reported by the CSI plugin.
      Resources discovered;
      hashset<string> discoveredVolumeIds;

      foreach (const VolumeInfo& volumeInfo, volumeInfos) {
        const Option<string> profile =
          checkpointedMap.contains(volumeInfo.id) &&
          checkpointedMap.at(volumeInfo.id).disk().source().has_profile()
            ? checkpointedMap.at(volumeInfo.id).disk().source().profile()
            : Option<string>::none();

        const Option<Labels> metadata = volumeInfo.context.empty()
          ? Option<Labels>::none()
          : convertStringMapToLabels(volumeInfo.context);

        const Resource resource = createRawDiskResource(
            info,
            volumeInfo.capacity,
            profile,
            vendor,
            volumeInfo.id,
            metadata);

        if (discoveredVolumeIds.contains(volumeInfo.id)) {
          LOG(WARNING) << "Dropping duplicated volume '" << resource
                       << "' from the discovered resources";

          continue;
        }

        discovered += resource;
        discoveredVolumeIds.insert(volumeInfo.id);

        if (checkpointedMap.contains(volumeInfo.id)) {
          const Resource& resource = checkpointedMap.at(volumeInfo.id);

          // If the volume context has been changed by a non-conforming CSI
          // plugin, the changes will be reflected in a resource conversion.
          if (resource.disk().source().metadata() !=
              metadata.getOrElse(Labels())) {
            toRemove += resource;

            Resource changed = resource;
            if (metadata.isSome()) {
              *changed.mutable_disk()->mutable_source()->mutable_metadata() =
                *metadata;
            } else {
              changed.mutable_disk()->mutable_source()->clear_metadata();
            }

            toAdd += changed;
          }
        }
      }

      ResourceConversion metadataConversion(
          std::move(toRemove), std::move(toAdd));

      Resources checkpointed = CHECK_NOTERROR(
          totalResources.filter([](const Resource& resource) {
            return resource.disk().source().has_id();
          }).apply(metadataConversion));

      return vector<ResourceConversion>{
        std::move(metadataConversion),
        computeConversion(std::move(checkpointed), std::move(discovered))};
    }));
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::getStoragePools()
{
  CHECK(info.has_id());

  vector<Future<Resource>> futures;

  foreachpair (const string& profile,
               const DiskProfileAdaptor::ProfileInfo& profileInfo,
               profileInfos) {
    futures.push_back(
        volumeManager->getCapacity(
            profileInfo.capability, profileInfo.parameters)
          .then(std::bind(
              &createRawDiskResource,
              info,
              lambda::_1,
              profile,
              vendor,
              None(),
              None())));
  }

  return collect(futures)
    .then(defer(self(), [=](const vector<Resource>& resources) {
      Resources discovered(resources); // Zero resources will be ignored.
      Resources checkpointed =
        totalResources.filter([](const Resource& resource) {
          return Resources::isDisk(resource, Resource::DiskInfo::Source::RAW) &&
            !resource.disk().source().has_id();
        });

      return vector<ResourceConversion>{
        computeConversion(std::move(checkpointed), std::move(discovered))};
    }));
}


void StorageLocalResourceProviderProcess::watchProfiles()
{
  auto err = [](const string& message) {
    LOG(ERROR) << "Failed to watch for DiskProfileAdaptor: " << message;
  };

  // TODO(chhsiao): Consider retrying with backoff.
  loop(
      self(),
      [=] {
        return diskProfileAdaptor->watch(profileInfos.keys(), info);
      },
      [=](const hashset<string>& profiles) {
        CHECK(info.has_id());

        LOG(INFO)
          << "Updating profiles " << stringify(profiles)
          << " for resource provider " << info.id();

        std::function<Future<Nothing>()> update = defer(self(), [=] {
          return updateProfiles(profiles)
            .then(defer(self(), &Self::reconcileResources, false));
        });

        // Update the profile mapping and storage pools in `sequence` to wait
        // for any pending operation that disallow reconciliation or the last
        // reconciliation (if any) to finish, and set up `reconciled` to drop
        // incoming operations that disallow reconciliation until the storage
        // pools are reconciled.
        reconciled = sequence.add(update);

        return reconciled
          .then(defer(self(), [=]() -> ControlFlow<Nothing> {
            return Continue();
          }));
      })
    .onFailed(std::bind(err, lambda::_1))
    .onDiscarded(std::bind(err, "future discarded"));
}


Future<Nothing> StorageLocalResourceProviderProcess::updateProfiles(
    const hashset<string>& profiles)
{
  // Remove disappeared profiles.
  foreach (const string& profile, profileInfos.keys()) {
    if (!profiles.contains(profile)) {
      profileInfos.erase(profile);
    }
  }

  // Translate and add newly appeared profiles.
  vector<Future<Nothing>> futures;
  foreach (const string& profile, profiles) {
    // Since profiles are immutable after creation, we do not need to
    // translate any profile that is already in the mapping.
    if (profileInfos.contains(profile)) {
      continue;
    }

    auto err = [](const string& profile, const string& message) {
      LOG(ERROR)
        << "Failed to translate profile '" << profile << "': " << message;
    };

    futures.push_back(diskProfileAdaptor->translate(profile, info)
      .then(defer(self(), [=](
          const DiskProfileAdaptor::ProfileInfo& profileInfo) {
        profileInfos.put(profile, profileInfo);
        return Nothing();
      }))
      .onFailed(std::bind(err, profile, lambda::_1))
      .onDiscarded(std::bind(err, profile, "future discarded")));
  }

  // We use `await` here to return a future that never fails, so the loop in
  // `watchProfiles` will continue to watch for profile changes. If any profile
  // translation fails, the profile will not be added to the set of known
  // profiles and thus the disk profile adaptor will notify the resource
  // provider again.
  return await(futures).then([] { return Nothing(); });
}


void StorageLocalResourceProviderProcess::subscribed(
    const Event::Subscribed& subscribed)
{
  CHECK_EQ(CONNECTED, state);

  LOG(INFO) << "Subscribed with ID " << subscribed.provider_id().value();

  state = SUBSCRIBED;

  if (!info.has_id()) {
    // New subscription.
    info.mutable_id()->CopyFrom(subscribed.provider_id());
    slave::paths::createResourceProviderDirectory(
        metaDir,
        slaveId,
        info.type(),
        info.name(),
        info.id());
  }

  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Failed to reconcile resource provider " << info.id() << ": "
      << message;
    fatal();
  };

  // Reconcile resources after obtaining the resource provider ID and start
  // watching for profile changes after the reconciliation.
  // TODO(chhsiao): Reconcile and watch for profile changes early.
  reconciled =
    reconcileResourceProviderState()
      .onReady(defer(self(), &Self::watchProfiles))
      .onReady(defer(self(), &Self::watchResources))
      .onFailed(defer(self(), std::bind(die, lambda::_1)))
      .onDiscarded(defer(self(), std::bind(die, "future discarded")));
}


void StorageLocalResourceProviderProcess::applyOperation(
    const Event::ApplyOperation& operation)
{
  CHECK(state == SUBSCRIBED || state == READY);

  Try<id::UUID> uuid = id::UUID::fromBytes(operation.operation_uuid().value());
  CHECK_SOME(uuid);

  LOG(INFO)
    << "Received " << operation.info().type() << " operation '"
    << operation.info().id() << "' (uuid: " << uuid.get() << ")";

  Option<FrameworkID> frameworkId = operation.has_framework_id()
    ? operation.framework_id() : Option<FrameworkID>::none();

  if (state == SUBSCRIBED) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operation.info(),
        "Cannot apply operation in SUBSCRIBED state");
  }

  if (reconciled.isPending() && !allowsReconciliation(operation.info())) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operation.info(),
        "Cannot apply operation when reconciling storage pools");
  }

  Try<id::UUID> operationVersion =
    id::UUID::fromBytes(operation.resource_version_uuid().value());
  CHECK_SOME(operationVersion);

  if (operationVersion.get() != resourceVersion) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operation.info(),
        "Mismatched resource version " + stringify(operationVersion.get()) +
        " (expected: " + stringify(resourceVersion) + ")");
  }

  CHECK(!operations.contains(uuid.get()));
  operations[uuid.get()] = protobuf::createOperation(
      operation.info(),
      protobuf::createOperationStatus(
          OPERATION_PENDING,
          operation.info().has_id()
            ? operation.info().id() : Option<OperationID>::none(),
          None(),
          None(),
          None(),
          slaveId,
          info.id()),
      frameworkId,
      slaveId,
      protobuf::createUUID(uuid.get()));

  checkpointResourceProviderState();

  ++metrics.operations_pending.at(operation.info().type());

  auto err = [](const id::UUID& uuid, const string& message) {
    LOG(ERROR)
      << "Failed to apply operation (uuid: " << uuid << "): " << message;
  };

  _applyOperation(uuid.get())
    .onFailed(std::bind(err, uuid.get(), lambda::_1))
    .onDiscarded(std::bind(err, uuid.get(), "future discarded"));
}


void StorageLocalResourceProviderProcess::publishResources(
    const Event::PublishResources& publish)
{
  Option<Error> error;
  hashset<string> volumeIds;

  if (state == SUBSCRIBED) {
    error = Error("Cannot publish resources in SUBSCRIBED state");
  } else {
    CHECK_EQ(READY, state);

    Resources resources = publish.resources();
    resources.unallocate();
    foreach (const Resource& resource, resources) {
      if (!totalResources.contains(resource)) {
        error = Error(
            "Cannot publish unknown resource '" + stringify(resource) + "'");
        break;
      }

      switch (resource.disk().source().type()) {
        case Resource::DiskInfo::Source::PATH:
        case Resource::DiskInfo::Source::MOUNT:
        case Resource::DiskInfo::Source::BLOCK: {
          CHECK(resource.disk().source().has_id());
          volumeIds.insert(resource.disk().source().id());
          break;
        }
        case Resource::DiskInfo::Source::UNKNOWN:
        case Resource::DiskInfo::Source::RAW: {
          error = Error(
              "Cannot publish volume of " +
              stringify(resource.disk().source().type()) + " type");
          break;
        }
      }
    }
  }

  Future<vector<Nothing>> allPublished;

  if (error.isSome()) {
    allPublished = Failure(error.get());
  } else {
    vector<Future<Nothing>> futures;

    foreach (const string& volumeId, volumeIds) {
      futures.push_back(volumeManager->publishVolume(volumeId));
    }

    allPublished = collect(futures);
  }

  allPublished
    .onAny(defer(self(), [=](const Future<vector<Nothing>>& future) {
      // TODO(chhsiao): Currently there is no way to reply to the
      // resource provider manager with a failure message, so we log the
      // failure here.
      if (!future.isReady()) {
        LOG(ERROR)
          << "Failed to publish resources '" << publish.resources() << "': "
          << (future.isFailed() ? future.failure() : "future discarded");
      }

      Call call;
      call.mutable_resource_provider_id()->CopyFrom(info.id());
      call.set_type(Call::UPDATE_PUBLISH_RESOURCES_STATUS);

      Call::UpdatePublishResourcesStatus* update =
        call.mutable_update_publish_resources_status();
      update->mutable_uuid()->CopyFrom(publish.uuid());
      update->set_status(future.isReady()
        ? Call::UpdatePublishResourcesStatus::OK
        : Call::UpdatePublishResourcesStatus::FAILED);

      auto err = [](const mesos::UUID& uuid, const string& message) {
        LOG(ERROR)
          << "Failed to send status update for publish "
          << id::UUID::fromBytes(uuid.value()).get() << ": " << message;
      };

      driver->send(evolve(call))
        .onFailed(std::bind(err, publish.uuid(), lambda::_1))
        .onDiscarded(std::bind(err, publish.uuid(), "future discarded"));
    }));
}


void StorageLocalResourceProviderProcess::acknowledgeOperationStatus(
    const Event::AcknowledgeOperationStatus& acknowledge)
{
  CHECK_EQ(READY, state);

  Try<id::UUID> operationUuid =
    id::UUID::fromBytes(acknowledge.operation_uuid().value());

  CHECK_SOME(operationUuid);

  Try<id::UUID> statusUuid =
    id::UUID::fromBytes(acknowledge.status_uuid().value());

  CHECK_SOME(statusUuid);

  auto err = [](const id::UUID& uuid, const string& message) {
    LOG(ERROR)
      << "Failed to acknowledge status update for operation (uuid: " << uuid
      << "): " << message;
  };

  // NOTE: It is possible that an incoming acknowledgement races with an
  // outgoing retry of status update, and then a duplicated
  // acknowledgement will be received. In this case, the following call
  // will fail, so we just leave an error log.
  statusUpdateManager.acknowledgement(operationUuid.get(), statusUuid.get())
    .then(defer(self(), [=](bool continuation) {
      if (!continuation) {
        operations.erase(operationUuid.get());
        checkpointResourceProviderState();
        garbageCollectOperationPath(operationUuid.get());
      }

      return Nothing();
    }))
    .onFailed(std::bind(err, operationUuid.get(), lambda::_1))
    .onDiscarded(std::bind(err, operationUuid.get(), "future discarded"));
}


void StorageLocalResourceProviderProcess::reconcileOperations(
    const Event::ReconcileOperations& reconcile)
{
  CHECK_EQ(READY, state);

  foreach (const mesos::UUID& operationUuid, reconcile.operation_uuids()) {
    Try<id::UUID> uuid = id::UUID::fromBytes(operationUuid.value());
    CHECK_SOME(uuid);

    if (operations.contains(uuid.get())) {
      // When the agent asks for reconciliation for a known operation,
      // that means the `APPLY_OPERATION` event races with the last
      // `UPDATE_STATE` call and arrives after the call. Since the event
      // is received, nothing needs to be done here.
      continue;
    }

    // TODO(chhsiao): Consider sending `OPERATION_UNKNOWN` instead.
    dropOperation(
        uuid.get(),
        None(),
        None(),
        "Unknown operation");
  }
}


Future<Nothing> StorageLocalResourceProviderProcess::_applyOperation(
    const id::UUID& operationUuid)
{
  CHECK(operations.contains(operationUuid));
  const Operation& operation = operations.at(operationUuid);

  CHECK(!protobuf::isTerminalState(operation.latest_status().state()));

  Future<vector<ResourceConversion>> conversions;

  switch (operation.info().type()) {
    case Offer::Operation::RESERVE:
    case Offer::Operation::UNRESERVE: {
      // Synchronously apply the speculative operations to ensure that its
      // result is reflected in the total resources before any of its succeeding
      // operations is applied.
      return updateOperationStatus(
          operationUuid,
          getResourceConversions(operation.info()));
    }
    case Offer::Operation::CREATE: {
      // Synchronously create the persistent volumes to ensure that its result
      // is reflected in the total resources before any of its succeeding
      // operations is applied.
      return updateOperationStatus(
          operationUuid, applyCreate(operation.info()));
    }
    case Offer::Operation::DESTROY: {
      // Synchronously clean up and destroy the persistent volumes to ensure
      // that its result is reflected in the total resources before any of its
      // succeeding operations is applied.
      return updateOperationStatus(
          operationUuid, applyDestroy(operation.info()));
    }
    case Offer::Operation::CREATE_DISK: {
      CHECK(operation.info().has_create_disk());

      conversions = applyCreateDisk(
          operation.info().create_disk().source(),
          operationUuid,
          operation.info().create_disk().target_type(),
          operation.info().create_disk().has_target_profile()
            ? operation.info().create_disk().target_profile()
            : Option<string>::none());

      break;
    }
    case Offer::Operation::DESTROY_DISK: {
      CHECK(operation.info().has_destroy_disk());

      conversions = applyDestroyDisk(
          operation.info().destroy_disk().source());

      break;
    }
    case Offer::Operation::GROW_VOLUME:
    case Offer::Operation::SHRINK_VOLUME: {
      // TODO(chhsiao): These operations are currently not supported for
      // resource providers, and should have been validated by the master.
      UNREACHABLE();
    }
    case Offer::Operation::UNKNOWN:
    case Offer::Operation::LAUNCH:
    case Offer::Operation::LAUNCH_GROUP: {
      UNREACHABLE();
    }
  }

  CHECK(!protobuf::isSpeculativeOperation(operation.info()))
    << "Unexpected speculative operation: " << operation.info().type();

  shared_ptr<Promise<Nothing>> promise(new Promise<Nothing>());

  conversions
    .onAny(defer(self(), [=](const Future<vector<ResourceConversion>>& future) {
      Try<vector<ResourceConversion>> conversions = future.isReady()
        ? Try<vector<ResourceConversion>>::some(future.get())
        : Error(future.isFailed() ? future.failure() : "future discarded");

      if (conversions.isSome()) {
        LOG(INFO)
          << "Applying conversion from '" << conversions->at(0).consumed
          << "' to '" << conversions->at(0).converted
          << "' for operation (uuid: " << operationUuid << ")";
      }

      promise->associate(
          updateOperationStatus(operationUuid, conversions));
    }));

  Future<Nothing> future = promise->future();

  if (!allowsReconciliation(operation.info())) {
    // We place the future in `sequence` so it can be waited before reconciling
    // storage pools.
    sequence.add(std::function<Future<Nothing>()>([future] { return future; }));
  }

  return future;
}


void StorageLocalResourceProviderProcess::dropOperation(
    const id::UUID& operationUuid,
    const Option<FrameworkID>& frameworkId,
    const Option<Offer::Operation>& operation,
    const string& message)
{
  LOG(WARNING)
    << "Dropping operation (uuid: " << operationUuid << "): " << message;

  CHECK(!operations.contains(operationUuid));

  UpdateOperationStatusMessage update =
    protobuf::createUpdateOperationStatusMessage(
        protobuf::createUUID(operationUuid),
        protobuf::createOperationStatus(
            OPERATION_DROPPED,
            None(),
            message,
            None(),
            None(),
            slaveId,
            info.id()),
        None(),
        frameworkId,
        slaveId);

  if (operation.isSome()) {
    // This operation is dropped intentionally. We have to persist the operation
    // in the resource provider state and retry the status update.
    *update.mutable_status()->mutable_uuid() = protobuf::createUUID();
    if (operation->has_id()) {
      *update.mutable_status()->mutable_operation_id() = operation->id();
    }

    operations[operationUuid] = protobuf::createOperation(
        operation.get(),
        update.status(),
        frameworkId,
        slaveId,
        update.operation_uuid());

    checkpointResourceProviderState();

    auto die = [=](const string& message) {
      LOG(ERROR)
        << "Failed to update status of operation (uuid: " << operationUuid
        << "): " << message;
      fatal();
    };

    statusUpdateManager.update(std::move(update))
      .onFailed(defer(self(), std::bind(die, lambda::_1)))
      .onDiscarded(defer(self(), std::bind(die, "future discarded")));
  } else {
    // This operation is unknown to the resource provider because of a
    // disconnection, and is being asked for reconciliation. In this case, we
    // send a status update without a retry. If it is dropped because of another
    // disconnection, another reconciliation will be triggered by the master
    // after a reregistration.
    sendOperationStatusUpdate(std::move(update));
  }

  ++metrics.operations_dropped.at(
      operation.isSome() ? operation->type() : Offer::Operation::UNKNOWN);
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyCreateDisk(
    const Resource& resource,
    const id::UUID& operationUuid,
    const Resource::DiskInfo::Source::Type& targetType,
    const Option<string>& targetProfile)
{
  CHECK_EQ(Resource::DiskInfo::Source::RAW, resource.disk().source().type());

  // NOTE: Currently we only support two types of RAW disk resources:
  //   1. RAW disk from `GetCapacity` with a profile but no volume ID.
  //   2. RAW disk from `ListVolumes` for a preprovisioned volume, which has a
  //      volume ID but no profile.
  //
  // For 1, we check if its profile is mount or block capable, then
  // call `createVolume` with the operation UUID as the name (so that
  // the same volume will be returned when recovering from a failover).
  //
  // For 2, the target profile will be specified, so we first check if the
  // profile is mount or block capable. Then, we call `validateVolume` to handle
  // the following two scenarios:
  //   a. If the volume has a checkpointed state (because it is created by a
  //      previous resource provider), we simply check if its checkpointed
  //      capability and parameters match the profile.
  //   b. If the volume is newly discovered, `ValidateVolumeCapabilities` is
  //      called with the capability of the profile.
  CHECK_NE(resource.disk().source().has_profile(),
           resource.disk().source().has_id() && targetProfile.isSome());

  const string profile =
    targetProfile.getOrElse(resource.disk().source().profile());

  if (!profileInfos.contains(profile)) {
    return Failure("Profile '" + profile + "' not found");
  }

  const DiskProfileAdaptor::ProfileInfo& profileInfo = profileInfos.at(profile);
  switch (targetType) {
    case Resource::DiskInfo::Source::MOUNT: {
      if (!profileInfo.capability.has_mount()) {
        return Failure(
            "Profile '" + profile + "' cannot be used to create a MOUNT disk");
      }
      break;
    }
    case Resource::DiskInfo::Source::BLOCK: {
      if (!profileInfo.capability.has_block()) {
        return Failure(
            "Profile '" + profile + "' cannot be used to create a BLOCK disk");
      }
      break;
    }
    case Resource::DiskInfo::Source::UNKNOWN:
    case Resource::DiskInfo::Source::PATH:
    case Resource::DiskInfo::Source::RAW: {
      UNREACHABLE();
    }
  }

  // TODO(chhsiao): Consider calling `createVolume` sequentially with other
  // create or delete operations, and send an `UPDATE_STATE` for storage pools
  // afterward.
  Future<VolumeInfo> created;
  if (resource.disk().source().has_profile()) {
    created = volumeManager->createVolume(
        operationUuid.toString(),
        resource.scalar().value() * Bytes::MEGABYTES,
        profileInfo.capability,
        profileInfo.parameters);
  } else {
    VolumeInfo volumeInfo = {
      resource.scalar().value() * Bytes::MEGABYTES,
      resource.disk().source().id(),
      CHECK_NOTERROR(convertLabelsToStringMap(
          resource.disk().source().metadata()))
    };

    created = volumeManager->validateVolume(
        volumeInfo, profileInfo.capability, profileInfo.parameters)
      .then([resource, profile, volumeInfo](
          const Option<Error>& error) -> Future<VolumeInfo> {
        if (error.isSome()) {
          return Failure(
              "Cannot apply profile '" + profile + "' to resource '" +
              stringify(resource) + "': " + error->message);
        }

        return volumeInfo;
      });
  }

  return created
    .then(defer(self(), [=](const VolumeInfo& volumeInfo) {
      Resource converted = resource;
      converted.mutable_disk()->mutable_source()->set_id(volumeInfo.id);
      converted.mutable_disk()->mutable_source()->set_type(targetType);
      converted.mutable_disk()->mutable_source()->set_profile(profile);

      if (!volumeInfo.context.empty()) {
        *converted.mutable_disk()->mutable_source()->mutable_metadata() =
          convertStringMapToLabels(volumeInfo.context);
      }

      const string mountRootDir = csi::paths::getMountRootDir(
          slave::paths::getCsiRootDir("."),
          info.storage().plugin().type(),
          info.storage().plugin().name());

      switch (targetType) {
        case Resource::DiskInfo::Source::MOUNT: {
          // Set the root path relative to agent work dir.
          converted.mutable_disk()->mutable_source()->mutable_mount()->set_root(
              mountRootDir);

          break;
        }
        case Resource::DiskInfo::Source::BLOCK: {
          break;
        }
        case Resource::DiskInfo::Source::UNKNOWN:
        case Resource::DiskInfo::Source::PATH:
        case Resource::DiskInfo::Source::RAW: {
          UNREACHABLE();
        }
      }

      vector<ResourceConversion> conversions;
      conversions.emplace_back(resource, std::move(converted));

      return conversions;
    }));
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyDestroyDisk(
    const Resource& resource)
{
  CHECK(!Resources::isPersistentVolume(resource));
  CHECK(resource.disk().source().has_id());

  return volumeManager->deleteVolume(resource.disk().source().id())
    .then(defer(self(), [=](bool deprovisioned) {
      Resource converted = resource;
      converted.mutable_disk()->mutable_source()->set_type(
          Resource::DiskInfo::Source::RAW);

      switch (resource.disk().source().type()) {
        case Resource::DiskInfo::Source::MOUNT: {
          converted.mutable_disk()->mutable_source()->clear_mount();
          break;
        }
        case Resource::DiskInfo::Source::BLOCK:
        case Resource::DiskInfo::Source::RAW: {
          break;
        }
        case Resource::DiskInfo::Source::UNKNOWN:
        case Resource::DiskInfo::Source::PATH: {
          UNREACHABLE(); // Should have been validated by the master.
        }
      }

      // We clear the volume ID and metadata if the volume has been
      // deprovisioned. Otherwise, we clear the profile.
      if (deprovisioned) {
        converted.mutable_disk()->mutable_source()->clear_id();
        converted.mutable_disk()->mutable_source()->clear_metadata();

        if (!resource.disk().source().has_profile() ||
            !profileInfos.contains(resource.disk().source().profile())) {
          // The destroyed volume is converted into an empty resource to prevent
          // the freed disk from being sent out with a disappeared profile.
          converted.mutable_scalar()->set_value(0);

          // Since the profile disappears, The freed disk might be claimed by
          // other appeared profiles. If there is an ongoing reconciliation, it
          // is waiting for this operation to finish and will recover the freed
          // disk, so no reconciliation should be done here. Otherwise, we
          // reconcile the storage pools to recover the freed disk.
          if (!reconciled.isPending()) {
            CHECK(info.has_id());

            LOG(INFO)
              << "Reconciling storage pools for resource provider " << info.id()
              << " after resource '" << resource << "' has been freed";

            // Reconcile the storage pools in `sequence` to wait for any other
            // pending operation that disallow reconciliation to finish, and set
            // up `reconciled` to drop incoming operations that disallow
            // reconciliation until the storage pools are reconciled.
            auto err = [](const Resource& resource, const string& message) {
              LOG(ERROR)
                << "Failed to reconcile storage pools after resource "
                << "'" << resource << "' has been freed: " << message;
            };

            reconciled =
              sequence
                .add(std::function<Future<Nothing>()>(
                    defer(self(), &Self::reconcileResources, false)))
                .onFailed(std::bind(err, resource, lambda::_1))
                .onDiscard(std::bind(err, resource, "future discarded"));
          }
        }
      } else {
        converted.mutable_disk()->mutable_source()->clear_profile();
      }

      vector<ResourceConversion> conversions;
      conversions.emplace_back(resource, std::move(converted));

      return conversions;
    }));
}


Try<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyCreate(
    const Offer::Operation& operation) const
{
  CHECK(operation.has_create());

  foreach (const Resource& resource, operation.create().volumes()) {
    CHECK(Resources::isPersistentVolume(resource));
    CHECK(resource.disk().source().has_id());

    // TODO(chhsiao): Support persistent BLOCK volumes.
    if (resource.disk().source().type() != Resource::DiskInfo::Source::MOUNT) {
      return Error(
          "Cannot create persistent volume '" +
          stringify(resource.disk().persistence().id()) + "' on a " +
          stringify(resource.disk().source().type()) + " disk");
    }

    // TODO(chhsiao): Ideally, we could perform a sanity check to verify that
    // the target path is empty before creating a new persistent volume.
    // However, right now we cannot distinguish the case where a framework is
    // recreating its own persistent volume after the agent ID changes from the
    // case where existing data is being leaked to another framework.
  }

  return getResourceConversions(operation);
}


Try<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyDestroy(
    const Offer::Operation& operation) const
{
  CHECK(operation.has_destroy());

  foreach (const Resource& resource, operation.destroy().volumes()) {
    CHECK(Resources::isPersistentVolume(resource));
    CHECK(resource.disk().source().has_id());

    // TODO(chhsiao): Support cleaning up persistent BLOCK volumes, presumably
    // with `dd` or any other utility to zero out the block device.
    CHECK_EQ(Resource::DiskInfo::Source::MOUNT,
             resource.disk().source().type());

    const string targetPath = csi::paths::getMountTargetPath(
        csi::paths::getMountRootDir(
            slave::paths::getCsiRootDir(workDir),
            info.storage().plugin().type(),
            info.storage().plugin().name()),
        resource.disk().source().id());

    if (os::exists(targetPath)) {
      // NOTE: We always clean up the data in the target path (but not the
      // directory itself) even if the volume is not published, in which case
      // this should be a no-op.
      Try<Nothing> rmdir = os::rmdir(targetPath, true, false);
      if (rmdir.isError()) {
        return Error(
            "Failed to remove persistent volume '" +
            stringify(resource.disk().persistence().id()) + "' at '" +
            targetPath + "': " + rmdir.error());
      }
    }
  }

  return getResourceConversions(operation);
}


Try<Nothing> StorageLocalResourceProviderProcess::updateOperationStatus(
    const id::UUID& operationUuid,
    const Try<vector<ResourceConversion>>& conversions)
{
  Option<Error> error;
  Resources convertedResources;

  CHECK(operations.contains(operationUuid));
  Operation& operation = operations.at(operationUuid);

  if (conversions.isSome()) {
    // Strip away the allocation info when applying the conversion to
    // the total resources.
    vector<ResourceConversion> _conversions;
    foreach (ResourceConversion conversion, conversions.get()) {
      convertedResources += conversion.converted;
      conversion.consumed.unallocate();
      conversion.converted.unallocate();
      _conversions.emplace_back(std::move(conversion));
    }

    Try<Resources> result = totalResources.apply(_conversions);
    if (result.isSome()) {
      totalResources = result.get();
    } else {
      error = result.error();
    }
  } else {
    error = conversions.error();
  }

  operation.mutable_latest_status()->CopyFrom(protobuf::createOperationStatus(
      error.isNone() ? OPERATION_FINISHED : OPERATION_FAILED,
      operation.info().has_id()
        ? operation.info().id() : Option<OperationID>::none(),
      error.isNone() ? Option<string>::none() : error->message,
      error.isNone() ? convertedResources : Option<Resources>::none(),
      id::UUID::random(),
      slaveId,
      info.id()));

  operation.add_statuses()->CopyFrom(operation.latest_status());

  checkpointResourceProviderState();

  // Send out the status update for the operation.
  UpdateOperationStatusMessage update =
    protobuf::createUpdateOperationStatusMessage(
        protobuf::createUUID(operationUuid),
        operation.latest_status(),
        None(),
        operation.has_framework_id()
          ? operation.framework_id() : Option<FrameworkID>::none(),
        slaveId);

  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Failed to update status of operation (uuid: " << operationUuid
      << "): " << message;
    fatal();
  };

  statusUpdateManager.update(std::move(update))
    .onFailed(defer(self(), std::bind(die, lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, "future discarded")));

  --metrics.operations_pending.at(operation.info().type());

  switch (operation.latest_status().state()) {
    case OPERATION_FINISHED:
      ++metrics.operations_finished.at(operation.info().type());
      break;
    case OPERATION_FAILED:
      ++metrics.operations_failed.at(operation.info().type());
      break;
    case OPERATION_UNSUPPORTED:
    case OPERATION_PENDING:
    case OPERATION_ERROR:
    case OPERATION_DROPPED:
    case OPERATION_UNREACHABLE:
    case OPERATION_GONE_BY_OPERATOR:
    case OPERATION_RECOVERING:
    case OPERATION_UNKNOWN:
      UNREACHABLE();
  }

  if (error.isSome()) {
    // We only send `UPDATE_STATE` for failed speculative operations.
    if (protobuf::isSpeculativeOperation(operation.info())) {
      resourceVersion = id::UUID::random();
      sendResourceProviderStateUpdate();
    }

    return error.get();
  }

  return Nothing();
}


void StorageLocalResourceProviderProcess::garbageCollectOperationPath(
    const id::UUID& operationUuid)
{
  CHECK(!operations.contains(operationUuid));

  const string path = slave::paths::getOperationPath(
      slave::paths::getResourceProviderPath(
          metaDir, slaveId, info.type(), info.name(), info.id()),
      operationUuid);

  if (os::exists(path)) {
    Try<Nothing> rmdir =  os::rmdir(path);
    if (rmdir.isError()) {
      LOG(ERROR)
        << "Failed to remove directory '" << path << "': " << rmdir.error();
    }
  }
}


void StorageLocalResourceProviderProcess::checkpointResourceProviderState()
{
  ResourceProviderState state;

  foreachvalue (const Operation& operation, operations) {
    state.add_operations()->CopyFrom(operation);
  }

  state.mutable_resources()->CopyFrom(totalResources);

  ResourceProviderState::Storage* storage = state.mutable_storage();

  // NOTE: We only checkpoint profiles associated with any storage
  // pool (i.e., resource that has no volume ID) in the total resources.
  // We do not need to checkpoint profiles for resources that have
  // volume IDs, as their volume capabilities are already checkpointed.
  hashset<string> requiredProfiles;
  foreach (const Resource& resource, totalResources) {
    if (!resource.disk().source().has_id()) {
      CHECK(resource.disk().source().has_profile());
      requiredProfiles.insert(resource.disk().source().profile());
    }
  }

  foreach (const string& profile, requiredProfiles) {
    CHECK(profileInfos.contains(profile));

    const DiskProfileAdaptor::ProfileInfo& profileInfo =
      profileInfos.at(profile);

    ResourceProviderState::Storage::ProfileInfo& profileInfo_ =
      (*storage->mutable_profiles())[profile];

    *profileInfo_.mutable_capability() = profileInfo.capability;
    *profileInfo_.mutable_parameters() = profileInfo.parameters;
  }

  const string statePath = slave::paths::getResourceProviderStatePath(
      metaDir, slaveId, info.type(), info.name(), info.id());

  // NOTE: We ensure the checkpoint is synced to the filesystem to avoid
  // resulting in a stale or empty checkpoint when a system crash happens.
  Try<Nothing> checkpoint = slave::state::checkpoint(statePath, state, true);
  CHECK_SOME(checkpoint)
    << "Failed to checkpoint resource provider state to '" << statePath << "': "
    << checkpoint.error();
}


void StorageLocalResourceProviderProcess::sendResourceProviderStateUpdate()
{
  // Set a new resource version here since we typically send state
  // updates when resources change. While this ensures we always have
  // a new resource version whenever we set new state, with that this
  // function is not idempotent anymore.
  resourceVersion = id::UUID::random();

  Call call;
  call.set_type(Call::UPDATE_STATE);
  call.mutable_resource_provider_id()->CopyFrom(info.id());

  Call::UpdateState* update = call.mutable_update_state();
  update->mutable_resources()->CopyFrom(totalResources);
  update->mutable_resource_version_uuid()->CopyFrom(
      protobuf::createUUID(resourceVersion));

  foreachvalue (const Operation& operation, operations) {
    update->add_operations()->CopyFrom(operation);
  }

  LOG(INFO)
    << "Sending UPDATE_STATE call with resources '" << totalResources
    << "' and " << update->operations_size() << " operations to agent "
    << slaveId;

  // NOTE: We terminate the resource provider here if the state cannot be
  // updated, so that the state is in sync with the agent's view.
  auto die = [=](const ResourceProviderID& id, const string& message) {
    LOG(ERROR)
      << "Failed to update state for resource provider " << id << ": "
      << message;
    fatal();
  };

  driver->send(evolve(call))
    .onFailed(defer(self(), std::bind(die, info.id(), lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, info.id(), "future discarded")));
}


void StorageLocalResourceProviderProcess::sendOperationStatusUpdate(
      const UpdateOperationStatusMessage& _update)
{
  Call call;
  call.set_type(Call::UPDATE_OPERATION_STATUS);
  call.mutable_resource_provider_id()->CopyFrom(info.id());

  Call::UpdateOperationStatus* update =
    call.mutable_update_operation_status();
  update->mutable_operation_uuid()->CopyFrom(_update.operation_uuid());
  update->mutable_status()->CopyFrom(_update.status());

  if (_update.has_framework_id()) {
    update->mutable_framework_id()->CopyFrom(_update.framework_id());
  }

  if (_update.has_latest_status()) {
    update->mutable_latest_status()->CopyFrom(_update.latest_status());
  }

  auto err = [](const id::UUID& uuid, const string& message) {
    LOG(ERROR)
      << "Failed to send status update for operation (uuid: " << uuid << "): "
      << message;
  };

  Try<id::UUID> uuid =
    id::UUID::fromBytes(_update.operation_uuid().value());

  CHECK_SOME(uuid);

  driver->send(evolve(call))
    .onFailed(std::bind(err, uuid.get(), lambda::_1))
    .onDiscarded(std::bind(err, uuid.get(), "future discarded"));
}


StorageLocalResourceProviderProcess::Metrics::Metrics(const string& prefix)
  : csi::Metrics(prefix)
{
  vector<Offer::Operation::Type> operationTypes;

  // NOTE: We use a switch statement here as a compile-time sanity check so we
  // won't forget to add metrics for new operations in the future.
  Offer::Operation::Type firstOperationType = Offer::Operation::RESERVE;
  switch (firstOperationType) {
    case Offer::Operation::RESERVE:
      operationTypes.push_back(Offer::Operation::RESERVE);
    case Offer::Operation::UNRESERVE:
      operationTypes.push_back(Offer::Operation::UNRESERVE);
    case Offer::Operation::CREATE:
      operationTypes.push_back(Offer::Operation::CREATE);
    case Offer::Operation::DESTROY:
      operationTypes.push_back(Offer::Operation::DESTROY);
    case Offer::Operation::CREATE_DISK:
      operationTypes.push_back(Offer::Operation::CREATE_DISK);
    case Offer::Operation::DESTROY_DISK:
      operationTypes.push_back(Offer::Operation::DESTROY_DISK);
      break;
    case Offer::Operation::GROW_VOLUME:
    case Offer::Operation::SHRINK_VOLUME:
      // TODO(chhsiao): These operations are currently not supported for
      // resource providers, and should have been validated by the master.
      UNREACHABLE();
    case Offer::Operation::UNKNOWN:
    case Offer::Operation::LAUNCH:
    case Offer::Operation::LAUNCH_GROUP:
      UNREACHABLE();
  };

  foreach (const Offer::Operation::Type& type, operationTypes) {
    const string name = strings::lower(Offer::Operation::Type_Name(type));

    operations_pending.put(type, PushGauge(
        prefix + "operations/" + name + "/pending"));
    operations_finished.put(type, Counter(
        prefix + "operations/" + name + "/finished"));
    operations_failed.put(type, Counter(
        prefix + "operations/" + name + "/failed"));
    operations_dropped.put(type, Counter(
        prefix + "operations/" + name + "/dropped"));

    process::metrics::add(operations_pending.at(type));
    process::metrics::add(operations_finished.at(type));
    process::metrics::add(operations_failed.at(type));
    process::metrics::add(operations_dropped.at(type));
  }

  // Special metric for counting the number of `OPERATION_DROPPED` statuses when
  // receiving explicit reconciliation for unknown operation UUIDs.
  operations_dropped.put(
      Offer::Operation::UNKNOWN,
      Counter(prefix + "operations/unknown/dropped"));

  process::metrics::add(operations_dropped.at(Offer::Operation::UNKNOWN));
}


StorageLocalResourceProviderProcess::Metrics::~Metrics()
{
  foreachvalue (const PushGauge& gauge, operations_pending) {
    process::metrics::remove(gauge);
  }

  foreachvalue (const Counter& counter, operations_finished) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, operations_failed) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, operations_dropped) {
    process::metrics::remove(counter);
  }
}


Try<Owned<LocalResourceProvider>> StorageLocalResourceProvider::create(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken,
    bool strict)
{
  Option<Error> error = validate(info);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<LocalResourceProvider>(new StorageLocalResourceProvider(
      url, workDir, info, slaveId, authToken, strict));
}


Option<Error> StorageLocalResourceProvider::validate(
    const ResourceProviderInfo& info)
{
  if (info.has_id()) {
    return Error("'ResourceProviderInfo.id' must not be set");
  }

  // Verify that the name follows Java package naming convention.
  // TODO(chhsiao): We should move this check to a validation function
  // for `ResourceProviderInfo`.
  if (!isValidName(info.name())) {
    return Error(
        "Resource provider name '" + info.name() +
        "' does not follow Java package naming convention");
  }

  if (!info.has_storage()) {
    return Error("'ResourceProviderInfo.storage' must be set");
  }

  // Verify that the type and name of the CSI plugin follow Java package
  // naming convention.
  // TODO(chhsiao): We should move this check to a validation function
  // for `CSIPluginInfo`.
  if (!isValidType(info.storage().plugin().type()) ||
      !isValidName(info.storage().plugin().name())) {
    return Error(
        "CSI plugin type '" + info.storage().plugin().type() +
        "' and name '" + info.storage().plugin().name() +
        "' does not follow Java package naming convention");
  }

  // Verify that the plugin provides the CSI node service.
  // TODO(chhsiao): We should move this check to a validation function
  // for `CSIPluginInfo`.
  bool hasNodeService = false;

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    if (container.services().end() != find(
            container.services().begin(),
            container.services().end(),
            csi::NODE_SERVICE)) {
      hasNodeService = true;
      break;
    }
  }

  if (!hasNodeService) {
    return Error(stringify(csi::NODE_SERVICE) + " not found");
  }

  return None();
}


StorageLocalResourceProvider::StorageLocalResourceProvider(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken,
    bool strict)
  : process(new StorageLocalResourceProviderProcess(
        url, workDir, info, slaveId, authToken, strict))
{
  spawn(CHECK_NOTNULL(process.get()));
}


StorageLocalResourceProvider::~StorageLocalResourceProvider()
{
  process::terminate(process.get());
  process::wait(process.get());
}

} // namespace internal {
} // namespace mesos {
