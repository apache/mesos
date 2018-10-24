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
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>
#include <process/timeout.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/push_gauge.hpp>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/resource_provider/resource_provider.hpp>
#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/realpath.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "csi/client.hpp"
#include "csi/paths.hpp"
#include "csi/rpc.hpp"
#include "csi/state.hpp"
#include "csi/utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/detector.hpp"
#include "resource_provider/state.hpp"

#include "slave/container_daemon.hpp"
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

using process::after;
using process::await;
using process::Break;
using process::collect;
using process::Continue;
using process::ControlFlow;
using process::defer;
using process::delay;
using process::dispatch;
using process::Failure;
using process::Future;
using process::loop;
using process::Owned;
using process::Process;
using process::Promise;
using process::Sequence;
using process::spawn;
using process::Timeout;

using process::http::authentication::Principal;

using process::metrics::Counter;
using process::metrics::PushGauge;

using mesos::csi::state::VolumeState;

using mesos::internal::protobuf::convertLabelsToStringMap;
using mesos::internal::protobuf::convertStringMapToLabels;

using mesos::internal::slave::ContainerDaemon;

using mesos::resource_provider::Call;
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


// Timeout for a CSI plugin component to create its endpoint socket.
// TODO(chhsiao): Make the timeout configurable.
static const Duration CSI_ENDPOINT_CREATION_TIMEOUT = Minutes(1);


// Returns the container ID of the standalone container to run a CSI plugin
// component. The container ID is of the following format:
//     <cid_prefix><csi_type>-<csi_name>--<list_of_services>
// where <cid_prefix> comes from the principal of the resource provider,
// <csi_type> and <csi_name> are the type and name of the CSI plugin, with dots
// replaced by dashes. <list_of_services> lists the CSI services provided by the
// component, concatenated with dashes.
static inline ContainerID getContainerId(
    const ResourceProviderInfo& info,
    const CSIPluginContainerInfo& container)
{
  const Principal principal = LocalResourceProvider::principal(info);
  CHECK(principal.claims.contains("cid_prefix"));

  string value = principal.claims.at("cid_prefix") + strings::join(
      "-",
      strings::replace(info.storage().plugin().type(), ".", "-"),
      info.storage().plugin().name(),
      "");

  for (int i = 0; i < container.services_size(); i++) {
    value += "-" + stringify(container.services(i));
  }

  ContainerID containerId;
  containerId.set_value(value);

  return containerId;
}


static Option<CSIPluginContainerInfo> getCSIPluginContainerInfo(
    const ResourceProviderInfo& info,
    const ContainerID& containerId)
{
  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    if (getContainerId(info, container) == containerId) {
      return container;
    }
  }

  return None();
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


// Returns the 'Bearer' credential as a header for calling the V1 agent
// API if the `authToken` is presented, or empty otherwise.
static inline http::Headers getAuthHeader(const Option<string>& authToken)
{
  http::Headers headers;

  if (authToken.isSome()) {
    headers["Authorization"] = "Bearer " + authToken.get();
  }

  return headers;
}


static inline Resource createRawDiskResource(
    const ResourceProviderInfo& info,
    const Bytes& capacity,
    const Option<string>& profile,
    const Option<string>& id = None(),
    const Option<Labels>& metadata = None())
{
  CHECK(info.has_id());

  Resource resource;
  resource.set_name("disk");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()
    ->set_value((double) capacity.bytes() / Bytes::MEGABYTES);
  resource.mutable_provider_id()->CopyFrom(info.id()),
  resource.mutable_reservations()->CopyFrom(info.default_reservations());
  resource.mutable_disk()->mutable_source()
    ->set_type(Resource::DiskInfo::Source::RAW);

  if (profile.isSome()) {
    resource.mutable_disk()->mutable_source()->set_profile(profile.get());
  }

  if (id.isSome()) {
    resource.mutable_disk()->mutable_source()->set_id(id.get());
  }

  if (metadata.isSome()) {
    resource.mutable_disk()->mutable_source()->mutable_metadata()
      ->CopyFrom(metadata.get());
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
      bool _strict)
    : ProcessBase(process::ID::generate("storage-local-resource-provider")),
      state(RECOVERING),
      url(_url),
      workDir(_workDir),
      metaDir(slave::paths::getMetaRootDir(_workDir)),
      contentType(ContentType::PROTOBUF),
      info(_info),
      slaveId(_slaveId),
      authToken(_authToken),
      strict(_strict),
      resourceVersion(id::UUID::random()),
      sequence("storage-local-resource-provider-sequence"),
      metrics("resource_providers/" + info.type() + "." + info.name() + "/")
  {
    diskProfileAdaptor = DiskProfileAdaptor::getAdaptor();
    CHECK_NOTNULL(diskProfileAdaptor.get());
  }

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const Event& event);

private:
  struct VolumeData
  {
    VolumeData(VolumeState&& _state)
      : state(_state), sequence(new Sequence("volume-sequence")) {}

    VolumeState state;

    // We run all CSI operations for the same volume on a sequence to
    // ensure that they are processed in a sequential order.
    Owned<Sequence> sequence;
  };

  void initialize() override;
  void fatal();

  // The recover functions are responsible to recover the state of the
  // resource provider and CSI volumes from checkpointed data.
  Future<Nothing> recover();
  Future<Nothing> recoverServices();
  Future<Nothing> recoverVolumes();
  Future<Nothing> recoverResourceProviderState();

  void doReliableRegistration();

  // The reconcile functions are responsible to reconcile the state of
  // the resource provider from the recovered state and other sources of
  // truth, such as CSI plugin responses or the status update manager.
  Future<Nothing> reconcileResourceProviderState();
  Future<Nothing> reconcileOperationStatuses();
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
  Future<Nothing> updateProfiles(const hashset<string>& profiles);

  // Reconcile the storage pools when the set of known profiles changes,
  // or a volume with an unknown profile is destroyed.
  Future<Nothing> reconcileStoragePools();

  // Returns true if the storage pools are allowed to be reconciled when
  // the operation is being applied.
  static bool allowsReconciliation(const Offer::Operation& operation);

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void applyOperation(const Event::ApplyOperation& operation);
  void publishResources(const Event::PublishResources& publish);
  void acknowledgeOperationStatus(
      const Event::AcknowledgeOperationStatus& acknowledge);
  void reconcileOperations(
      const Event::ReconcileOperations& reconcile);

  template <csi::v0::RPC rpc>
  Future<typename csi::v0::RPCTraits<rpc>::response_type> call(
      csi::v0::Client client,
      typename csi::v0::RPCTraits<rpc>::request_type&& request);

  Future<csi::v0::Client> connect(const string& endpoint);
  Future<csi::v0::Client> getService(const ContainerID& containerId);
  Future<hashmap<ContainerID, Option<ContainerStatus>>> getContainers();
  Future<Nothing> waitContainer(const ContainerID& containerId);
  Future<Nothing> killContainer(const ContainerID& containerId);

  Future<Nothing> prepareIdentityService();
  Future<Nothing> prepareControllerService();
  Future<Nothing> prepareNodeService();
  Future<Nothing> controllerPublish(const string& volumeId);
  Future<Nothing> controllerUnpublish(const string& volumeId);
  Future<Nothing> nodeStage(const string& volumeId);
  Future<Nothing> nodeUnstage(const string& volumeId);
  Future<Nothing> nodePublish(const string& volumeId);
  Future<Nothing> nodeUnpublish(const string& volumeId);
  Future<string> createVolume(
      const string& name,
      const Bytes& capacity,
      const DiskProfileAdaptor::ProfileInfo& profileInfo);
  Future<Nothing> deleteVolume(const string& volumeId, bool preExisting);
  Future<string> validateCapability(
      const string& volumeId,
      const Option<Labels>& metadata,
      const csi::v0::VolumeCapability& capability);
  Future<Resources> listVolumes();
  Future<Resources> getCapacities();

  Future<Nothing> _applyOperation(const id::UUID& operationUuid);
  void dropOperation(
      const id::UUID& operationUuid,
      const Option<FrameworkID>& frameworkId,
      const Option<Offer::Operation>& operation,
      const string& message);

  Future<vector<ResourceConversion>> applyCreateDisk(
      const Resource& resource,
      const id::UUID& operationUuid,
      const Resource::DiskInfo::Source::Type& type);
  Future<vector<ResourceConversion>> applyDestroyDisk(
      const Resource& resource);

  Try<Nothing> updateOperationStatus(
      const id::UUID& operationUuid,
      const Try<vector<ResourceConversion>>& conversions);

  void garbageCollectOperationPath(const id::UUID& operationUuid);

  void checkpointResourceProviderState();
  void checkpointVolumeState(const string& volumeId);

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

  const http::URL url;
  const string workDir;
  const string metaDir;
  const ContentType contentType;
  ResourceProviderInfo info;
  const SlaveID slaveId;
  const Option<string> authToken;
  const bool strict;

  shared_ptr<DiskProfileAdaptor> diskProfileAdaptor;

  csi::v0::VolumeCapability defaultMountCapability;
  csi::v0::VolumeCapability defaultBlockCapability;
  string bootId;
  process::grpc::client::Runtime runtime;
  Owned<v1::resource_provider::Driver> driver;
  OperationStatusUpdateManager statusUpdateManager;

  // The mapping of known profiles fetched from the DiskProfileAdaptor.
  hashmap<string, DiskProfileAdaptor::ProfileInfo> profileInfos;

  hashmap<ContainerID, Owned<ContainerDaemon>> daemons;
  hashmap<ContainerID, Owned<Promise<csi::v0::Client>>> services;

  Option<ContainerID> nodeContainerId;
  Option<ContainerID> controllerContainerId;
  Option<csi::v0::GetPluginInfoResponse> pluginInfo;
  csi::v0::PluginCapabilities pluginCapabilities;
  csi::v0::ControllerCapabilities controllerCapabilities;
  csi::v0::NodeCapabilities nodeCapabilities;
  Option<string> nodeId;

  // We maintain the following invariant: if one operation depends on
  // another, they cannot be in PENDING state at the same time, i.e.,
  // the result of the preceding operation must have been reflected in
  // the total resources.
  // NOTE: We store the list of operations in a `LinkedHashMap` to
  // preserve the order we receive the operations in case we need it.
  LinkedHashMap<id::UUID, Operation> operations;
  Resources totalResources;
  id::UUID resourceVersion;
  hashmap<string, VolumeData> volumes;

  // If pending, it means that the storage pools are being reconciled, and all
  // incoming operations that disallow reconciliation will be dropped.
  Future<Nothing> reconciled;

  // We maintain a sequence to coordinate reconciliations of storage pools. It
  // keeps track of pending operations that disallow reconciliation, and ensures
  // that any reconciliation waits for these operations to finish.
  Sequence sequence;

  struct Metrics
  {
    explicit Metrics(const string& prefix);
    ~Metrics();

    // CSI plugin metrics.
    Counter csi_plugin_container_terminations;
    hashmap<csi::v0::RPC, PushGauge> csi_plugin_rpcs_pending;
    hashmap<csi::v0::RPC, Counter> csi_plugin_rpcs_successes;
    hashmap<csi::v0::RPC, Counter> csi_plugin_rpcs_errors;
    hashmap<csi::v0::RPC, Counter> csi_plugin_rpcs_cancelled;

    // Operation state metrics.
    hashmap<Offer::Operation::Type, PushGauge> operations_pending;
    hashmap<Offer::Operation::Type, Counter> operations_finished;
    hashmap<Offer::Operation::Type, Counter> operations_failed;
    hashmap<Offer::Operation::Type, Counter> operations_dropped;
  } metrics;
};


void StorageLocalResourceProviderProcess::connected()
{
  CHECK_EQ(DISCONNECTED, state);

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
  // Default mount and block capabilities for pre-existing volumes.
  defaultMountCapability.mutable_mount();
  defaultMountCapability.mutable_access_mode()
    ->set_mode(csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);
  defaultBlockCapability.mutable_block();
  defaultBlockCapability.mutable_access_mode()
    ->set_mode(csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);

  Try<string> _bootId = os::bootId();
  if (_bootId.isError()) {
    LOG(ERROR) << "Failed to get boot ID: " << _bootId.error();
    return fatal();
  }

  bootId = _bootId.get();

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    if (container.services().end() != find(
            container.services().begin(),
            container.services().end(),
            CSIPluginContainerInfo::NODE_SERVICE)) {
      nodeContainerId = getContainerId(info, container);
      break;
    }
  }

  CHECK_SOME(nodeContainerId);

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    if (container.services().end() != find(
            container.services().begin(),
            container.services().end(),
            CSIPluginContainerInfo::CONTROLLER_SERVICE)) {
      controllerContainerId = getContainerId(info, container);
      break;
    }
  }

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

  return recoverServices()
    .then(defer(self(), &Self::recoverVolumes))
    .then(defer(self(), &Self::recoverResourceProviderState))
    .then(defer(self(), [=]() -> Future<Nothing> {
      LOG(INFO)
        << "Finished recovery for resource provider with type '" << info.type()
        << "' and name '" << info.name() << "'";

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


Future<Nothing> StorageLocalResourceProviderProcess::recoverServices()
{
  return getContainers()
    .then(defer(self(), [=](
        const hashmap<ContainerID, Option<ContainerStatus>>& runningContainers)
        -> Future<Nothing> {
      Try<list<string>> containerPaths = csi::paths::getContainerPaths(
          slave::paths::getCsiRootDir(workDir),
          info.storage().plugin().type(),
          info.storage().plugin().name());

      if (containerPaths.isError()) {
        return Failure(
            "Failed to find plugin containers for CSI plugin type '" +
            info.storage().plugin().type() + "' and name '" +
            info.storage().plugin().name() + "': " +
            containerPaths.error());
      }

      vector<Future<Nothing>> futures;

      foreach (const string& path, containerPaths.get()) {
        Try<csi::paths::ContainerPath> containerPath =
          csi::paths::parseContainerPath(
              slave::paths::getCsiRootDir(workDir),
              path);

        if (containerPath.isError()) {
          return Failure(
              "Failed to parse container path '" + path + "': " +
              containerPath.error());
        }

        CHECK_EQ(info.storage().plugin().type(), containerPath->type);
        CHECK_EQ(info.storage().plugin().name(), containerPath->name);

        const ContainerID& containerId = containerPath->containerId;

        // NOTE: Since `getContainers` might return containers that are not
        // actually running, to identify if the container is actually running,
        // we check if the `executor_pid` field is set as a workaround.
        bool isRunningContainer = runningContainers.contains(containerId) &&
          runningContainers.at(containerId).isSome() &&
          runningContainers.at(containerId)->has_executor_pid();

        // Do not kill the up-to-date running controller or node container.
        if ((nodeContainerId == containerId ||
             controllerContainerId == containerId) && isRunningContainer) {
          const string configPath = csi::paths::getContainerInfoPath(
              slave::paths::getCsiRootDir(workDir),
              info.storage().plugin().type(),
              info.storage().plugin().name(),
              containerId);

          if (os::exists(configPath)) {
            Result<CSIPluginContainerInfo> config =
              slave::state::read<CSIPluginContainerInfo>(configPath);

            if (config.isError()) {
              return Failure(
                  "Failed to read plugin container config from '" + configPath +
                  "': " + config.error());
            }

            if (config.isSome() &&
                getCSIPluginContainerInfo(info, containerId) == config.get()) {
              continue;
            }
          }
        }

        LOG(INFO) << "Cleaning up plugin container '" << containerId << "'";

        // Otherwise, kill the container only if it is actually running (i.e.,
        // not already being destroyed), then wait for the container to be
        // destroyed before performing the cleanup despite if we kill it.
        Future<Nothing> cleanup = Nothing();
        if (runningContainers.contains(containerId)) {
          if (isRunningContainer) {
            cleanup = killContainer(containerId);
          }
          cleanup = cleanup
            .then(defer(self(), &Self::waitContainer, containerId));
        }

        cleanup = cleanup
          .then(defer(self(), [=]() -> Future<Nothing> {
            Result<string> endpointDir =
              os::realpath(csi::paths::getEndpointDirSymlinkPath(
                  slave::paths::getCsiRootDir(workDir),
                  info.storage().plugin().type(),
                  info.storage().plugin().name(),
                  containerId));

            if (endpointDir.isSome()) {
              Try<Nothing> rmdir = os::rmdir(endpointDir.get());
              if (rmdir.isError()) {
                return Failure(
                    "Failed to remove endpoint directory '" +
                    endpointDir.get() + "': " + rmdir.error());
              }
            }

            Try<Nothing> rmdir = os::rmdir(path);
                if (rmdir.isError()) {
              return Failure(
                  "Failed to remove plugin container directory '" + path +
                  "': " + rmdir.error());
            }

            return Nothing();
          }));

        futures.push_back(cleanup);
      }

      return collect(futures).then([] { return Nothing(); });
    }))
    // NOTE: The `Controller` service is supported if the plugin has the
    // `CONTROLLER_SERVICE` capability, and the `NodeGetId` call is supported if
    // the `Controller` service has the `PUBLISH_UNPUBLISH_VOLUME` capability.
    // So we first launch the node plugin to get the plugin capabilities, then
    // decide if we need to launch the controller plugin and get the node ID.
    .then(defer(self(), &Self::prepareIdentityService))
    .then(defer(self(), &Self::prepareControllerService))
    .then(defer(self(), &Self::prepareNodeService));
}


Future<Nothing> StorageLocalResourceProviderProcess::recoverVolumes()
{
  // Recover the states of CSI volumes.
  Try<list<string>> volumePaths = csi::paths::getVolumePaths(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name());

  if (volumePaths.isError()) {
    return Failure(
        "Failed to find volumes for CSI plugin type '" +
        info.storage().plugin().type() + "' and name '" +
        info.storage().plugin().name() + "': " + volumePaths.error());
  }

  vector<Future<Nothing>> futures;

  foreach (const string& path, volumePaths.get()) {
    Try<csi::paths::VolumePath> volumePath =
      csi::paths::parseVolumePath(slave::paths::getCsiRootDir(workDir), path);

    if (volumePath.isError()) {
      return Failure(
          "Failed to parse volume path '" + path + "': " + volumePath.error());
    }

    CHECK_EQ(info.storage().plugin().type(), volumePath->type);
    CHECK_EQ(info.storage().plugin().name(), volumePath->name);

    const string& volumeId = volumePath->volumeId;
    const string statePath = csi::paths::getVolumeStatePath(
        slave::paths::getCsiRootDir(workDir),
        info.storage().plugin().type(),
        info.storage().plugin().name(),
        volumeId);

    if (!os::exists(statePath)) {
      continue;
    }

    Result<VolumeState> volumeState =
      slave::state::read<VolumeState>(statePath);

    if (volumeState.isError()) {
      return Failure(
          "Failed to read volume state from '" + statePath + "': " +
          volumeState.error());
    }

    if (volumeState.isSome()) {
      volumes.put(volumeId, std::move(volumeState.get()));
      VolumeData& volume = volumes.at(volumeId);

      Future<Nothing> recovered = Nothing();

      if (VolumeState::State_IsValid(volume.state.state())) {
        switch (volume.state.state()) {
          case VolumeState::CREATED:
          case VolumeState::NODE_READY: {
            break;
          }
          case VolumeState::VOL_READY:
          case VolumeState::PUBLISHED: {
            if (volume.state.boot_id() != bootId) {
              // The node has been restarted since the volume is made
              // publishable, so it is reset to `NODE_READY` state.
              volume.state.set_state(VolumeState::NODE_READY);
              volume.state.clear_boot_id();
              checkpointVolumeState(volumeId);
            }

            break;
          }
          case VolumeState::CONTROLLER_PUBLISH: {
            recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                defer(self(), &Self::controllerPublish, volumeId)));

            break;
          }
          case VolumeState::CONTROLLER_UNPUBLISH: {
            recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                defer(self(), &Self::controllerUnpublish, volumeId)));

            break;
          }
          case VolumeState::NODE_STAGE: {
            recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                defer(self(), &Self::nodeStage, volumeId)));

            break;
          }
          case VolumeState::NODE_UNSTAGE: {
            recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                defer(self(), &Self::nodeUnstage, volumeId)));

            break;
          }
          case VolumeState::NODE_PUBLISH: {
            if (volume.state.boot_id() != bootId) {
              // The node has been restarted since `NodePublishVolume` was
              // called, so it is reset to `NODE_READY` state.
              volume.state.set_state(VolumeState::NODE_READY);
              volume.state.clear_boot_id();
              checkpointVolumeState(volumeId);
            } else {
              recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                  defer(self(), &Self::nodePublish, volumeId)));
            }

            break;
          }
          case VolumeState::NODE_UNPUBLISH: {
            if (volume.state.boot_id() != bootId) {
              // The node has been restarted since `NodeUnpublishVolume` was
              // called, so it is reset to `NODE_READY` state.
              volume.state.set_state(VolumeState::NODE_READY);
              volume.state.clear_boot_id();
              checkpointVolumeState(volumeId);
            } else {
              recovered = volume.sequence->add(std::function<Future<Nothing>()>(
                  defer(self(), &Self::nodeUnpublish, volumeId)));
            }

            break;
          }
          case VolumeState::UNKNOWN: {
            recovered = Failure(
                "Volume '" + volumeId + "' is in " +
                stringify(volume.state.state()) + " state");

            break;
          }

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
          case google::protobuf::kint32min:
          case google::protobuf::kint32max: {
            UNREACHABLE();
          }
        }
      } else {
        recovered = Failure("Volume '" + volumeId + "' is in UNDEFINED state");
      }

      futures.push_back(recovered);
    }
  }

  return collect(futures).then([] { return Nothing(); });
}


Future<Nothing>
StorageLocalResourceProviderProcess::recoverResourceProviderState()
{
  // Recover the resource provider ID and state from the latest
  // symlink. If the symlink does not exist, this is a new resource
  // provider, and the total resources will be empty, which is fine
  // since new resources will be added during reconciliation.
  Result<string> realpath = os::realpath(
      slave::paths::getLatestResourceProviderPath(
          metaDir, slaveId, info.type(), info.name()));

  if (realpath.isError()) {
    return Failure(
        "Failed to read the latest symlink for resource provider with type '" +
        info.type() + "' and name '" + info.name() + "': " + realpath.error());
  }

  if (realpath.isSome()) {
    info.mutable_id()->set_value(Path(realpath.get()).basename());

    const string statePath = slave::paths::getResourceProviderStatePath(
        metaDir, slaveId, info.type(), info.name(), info.id());

    if (!os::exists(statePath)) {
      return Nothing();
    }

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
        Try<id::UUID> uuid = id::UUID::fromBytes(operation.uuid().value());

        CHECK_SOME(uuid);

        operations[uuid.get()] = operation;
      }

      totalResources = resourceProviderState->resources();

      const ResourceProviderState::Storage& storage =
        resourceProviderState->storage();

      using ProfileEntry = google::protobuf::MapPair<
          string, ResourceProviderState::Storage::ProfileInfo>;

      foreach (const ProfileEntry& entry, storage.profiles()) {
        profileInfos.put(
            entry.first,
            {entry.second.capability(), entry.second.parameters()});
      }

      // We only checkpoint profiles associated with storage pools (i.e.,
      // resources without IDs) in `checkpointResourceProviderState` as only
      // these profiles might be used by pending operations, so we validate here
      // that all such profiles exist.
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

  return Nothing();
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
    .then(defer(self(), [=] {
      return collect(vector<Future<Resources>>{listVolumes(), getCapacities()})
        .then(defer(self(), [=](const vector<Resources>& discovered) {
          ResourceConversion conversion = reconcileResources(
              totalResources,
              accumulate(discovered.begin(), discovered.end(), Resources()));

          Try<Resources> result = totalResources.apply(conversion);
          CHECK_SOME(result);

          if (result.get() != totalResources) {
            LOG(INFO)
              << "Removing '" << conversion.consumed << "' and adding '"
              << conversion.converted << "' to the total resources";

            totalResources = result.get();
            checkpointResourceProviderState();
          }

          // NOTE: Since this is the first `UPDATE_STATE` call of the
          // current subscription, there must be no racing speculative
          // operation, thus no need to update the resource version.
          sendResourceProviderStateUpdate();
          statusUpdateManager.resume();

          LOG(INFO)
            << "Resource provider " << info.id() << " is in READY state";

          state = READY;

          return Nothing();
        }));
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
          case OPERATION_UNSUPPORTED:
          case OPERATION_ERROR:
          case OPERATION_DROPPED:
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


Future<Nothing> StorageLocalResourceProviderProcess::reconcileStoragePools()
{
  CHECK_PENDING(reconciled);

  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Failed to reconcile storage pools for resource provider " << info.id()
      << ": " << message;
    fatal();
  };

  return getCapacities()
    .then(defer(self(), [=](const Resources& discovered) {
      ResourceConversion conversion = reconcileResources(
          totalResources.filter(
              [](const Resource& r) { return !r.disk().source().has_id(); }),
          discovered);

      Try<Resources> result = totalResources.apply(conversion);
      CHECK_SOME(result);

      if (result.get() != totalResources) {
        LOG(INFO)
          << "Removing '" << conversion.consumed << "' and adding '"
          << conversion.converted << "' to the total resources";

        totalResources = result.get();
        checkpointResourceProviderState();

        // NOTE: We always update the resource version before sending
        // an `UPDATE_STATE`, so that any racing speculative operation
        // will be rejected. Otherwise, the speculative resource
        // conversion done on the master will be cancelled out.
        resourceVersion = id::UUID::random();
        sendResourceProviderStateUpdate();
      }

      return Nothing();
    }))
    .onFailed(defer(self(), std::bind(die, lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, "future discarded")));
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


ResourceConversion StorageLocalResourceProviderProcess::reconcileResources(
    const Resources& checkpointed,
    const Resources& discovered)
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
    Resource unconverted = createRawDiskResource(
        info,
        Bytes(resource.scalar().value() * Bytes::MEGABYTES),
        resource.disk().source().has_profile()
          ? resource.disk().source().profile() : Option<string>::none(),
        resource.disk().source().has_id()
          ? resource.disk().source().id() : Option<string>::none(),
        resource.disk().source().has_metadata()
          ? resource.disk().source().metadata() : Option<Labels>::none());

    if (toAdd.contains(unconverted)) {
      // If the remaining of the discovered resources contain the
      // "unconverted" version of a checkpointed resource, this is not a
      // new resource.
      toAdd -= unconverted;
    } else if (checkpointed.contains(unconverted)) {
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
            .then(defer(self(), &Self::reconcileStoragePools));
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
  reconciled = reconcileResourceProviderState()
    .onReady(defer(self(), &Self::watchProfiles))
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
            ? operation.info().id() : Option<OperationID>::none()),
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
          CHECK(volumes.contains(resource.disk().source().id()));
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
      // We check the state of the volume along with the CSI calls
      // atomically with respect to other publish or deletion requests
      // for the same volume through dispatching the whole lambda on the
      // volume's sequence.
      std::function<Future<Nothing>()> controllerAndNodePublish =
        defer(self(), [=] {
          CHECK(volumes.contains(volumeId));
          const VolumeData& volume = volumes.at(volumeId);

          Future<Nothing> published = Nothing();

          CHECK(VolumeState::State_IsValid(volume.state.state()));

          switch (volume.state.state()) {
            case VolumeState::CONTROLLER_UNPUBLISH: {
              published = published
                .then(defer(self(), &Self::controllerUnpublish, volumeId));

              // NOTE: We continue to the next case to publish the volume in
              // `CREATED` state once the above is done.
            }
            case VolumeState::CREATED:
            case VolumeState::CONTROLLER_PUBLISH: {
              published = published
                .then(defer(self(), &Self::controllerPublish, volumeId))
                .then(defer(self(), &Self::nodeStage, volumeId))
                .then(defer(self(), &Self::nodePublish, volumeId));

              break;
            }
            case VolumeState::NODE_UNSTAGE: {
              published = published
                .then(defer(self(), &Self::nodeUnstage, volumeId));

              // NOTE: We continue to the next case to publish the volume in
              // `NODE_READY` state once the above is done.
            }
            case VolumeState::NODE_READY:
            case VolumeState::NODE_STAGE: {
              published = published
                .then(defer(self(), &Self::nodeStage, volumeId))
                .then(defer(self(), &Self::nodePublish, volumeId));

              break;
            }
            case VolumeState::NODE_UNPUBLISH: {
              published = published
                .then(defer(self(), &Self::nodeUnpublish, volumeId));

              // NOTE: We continue to the next case to publish the volume in
              // `VOL_READY` state once the above is done.
            }
            case VolumeState::VOL_READY:
            case VolumeState::NODE_PUBLISH: {
              published = published
                .then(defer(self(), &Self::nodePublish, volumeId));

              break;
            }
            case VolumeState::PUBLISHED: {
              break;
            }
            case VolumeState::UNKNOWN: {
              UNREACHABLE();
            }

            // NOTE: We avoid using a default clause for the following
            // values in proto3's open enum to enable the compiler to detect
            // missing enum cases for us. See:
            // https://github.com/google/protobuf/issues/3917
            case google::protobuf::kint32min:
            case google::protobuf::kint32max: {
              UNREACHABLE();
            }
          }

          return published;
        });

      futures.push_back(
          volumes.at(volumeId).sequence->add(controllerAndNodePublish));
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

    dropOperation(
        uuid.get(),
        None(),
        None(),
        "Unknown operation");
  }
}


template <csi::v0::RPC rpc>
Future<typename csi::v0::RPCTraits<rpc>::response_type>
StorageLocalResourceProviderProcess::call(
    csi::v0::Client client,
    typename csi::v0::RPCTraits<rpc>::request_type&& request)
{
  ++metrics.csi_plugin_rpcs_pending.at(rpc);

  return client.call<rpc>(std::move(request))
    .onAny(defer(self(), [=](
        const Future<typename csi::v0::RPCTraits<rpc>::response_type>& future) {
      --metrics.csi_plugin_rpcs_pending.at(rpc);
      if (future.isReady()) {
        ++metrics.csi_plugin_rpcs_successes.at(rpc);
      } else if (future.isFailed()) {
        ++metrics.csi_plugin_rpcs_errors.at(rpc);
      } else {
        ++metrics.csi_plugin_rpcs_cancelled.at(rpc);
      }
    }));
}


// Returns a future of a CSI client that waits for the endpoint socket
// to appear if necessary, then connects to the socket and check its
// readiness.
Future<csi::v0::Client> StorageLocalResourceProviderProcess::connect(
    const string& endpoint)
{
  Future<csi::v0::Client> future;

  if (os::exists(endpoint)) {
    future = csi::v0::Client("unix://" + endpoint, runtime);
  } else {
    // Wait for the endpoint socket to appear until the timeout expires.
    Timeout timeout = Timeout::in(CSI_ENDPOINT_CREATION_TIMEOUT);

    future = loop(
        self(),
        [=]() -> Future<Nothing> {
          if (timeout.expired()) {
            return Failure("Timed out waiting for endpoint '" + endpoint + "'");
          }

          return after(Milliseconds(10));
        },
        [=](const Nothing&) -> ControlFlow<csi::v0::Client> {
          if (os::exists(endpoint)) {
            return Break(csi::v0::Client("unix://" + endpoint, runtime));
          }

          return Continue();
        });
  }

  return future
    .then(defer(self(), [=](csi::v0::Client client) {
      return call<csi::v0::PROBE>(client, csi::v0::ProbeRequest())
        .then(defer(self(), [=](
            const csi::v0::ProbeResponse& response) -> csi::v0::Client {
          return client;
        }));
    }));
}


// Returns a future of the latest CSI client for the specified plugin
// container. If the container is not already running, this method will
// start a new a new container daemon.
Future<csi::v0::Client> StorageLocalResourceProviderProcess::getService(
    const ContainerID& containerId)
{
  if (daemons.contains(containerId)) {
    CHECK(services.contains(containerId));
    return services.at(containerId)->future();
  }

  Option<CSIPluginContainerInfo> config =
    getCSIPluginContainerInfo(info, containerId);
  CHECK_SOME(config);

  // We checkpoint the config first to keep track of the plugin container even
  // if we fail to create its container daemon.
  const string configPath = csi::paths::getContainerInfoPath(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name(),
      containerId);

  Try<Nothing> checkpoint = slave::state::checkpoint(configPath, config.get());
  if (checkpoint.isError()) {
    return Failure(
        "Failed to checkpoint plugin container config to '" + configPath +
        "': " + checkpoint.error());
  }

  CommandInfo commandInfo;
  if (config->has_command()) {
    commandInfo.CopyFrom(config->command());
  }

  // Set the `CSI_ENDPOINT` environment variable.
  Try<string> endpoint = csi::paths::getEndpointSocketPath(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name(),
      containerId);

  if (endpoint.isError()) {
    return Failure(
        "Failed to resolve endpoint path for plugin container '" +
        stringify(containerId) + "': " + endpoint.error());
  }

  const string& endpointPath = endpoint.get();
  Environment::Variable* endpointVar =
    commandInfo.mutable_environment()->add_variables();
  endpointVar->set_name("CSI_ENDPOINT");
  endpointVar->set_value("unix://" + endpointPath);

  ContainerInfo containerInfo;
  if (config->has_container()) {
    containerInfo.CopyFrom(config->container());
  } else {
    containerInfo.set_type(ContainerInfo::MESOS);
  }

  // Prepare a volume where the endpoint socket will be placed.
  const string endpointDir = Path(endpointPath).dirname();
  Volume* endpointVolume = containerInfo.add_volumes();
  endpointVolume->set_mode(Volume::RW);
  endpointVolume->set_container_path(endpointDir);
  endpointVolume->set_host_path(endpointDir);

  // Prepare the directory where the mount points will be placed.
  const string mountRootDir = csi::paths::getMountRootDir(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name());

  Try<Nothing> mkdir = os::mkdir(mountRootDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" + mountRootDir + "': " + mkdir.error());
  }

  // Prepare a volume where the mount points will be placed.
  Volume* mountVolume = containerInfo.add_volumes();
  mountVolume->set_mode(Volume::RW);
  mountVolume->set_container_path(mountRootDir);
  mountVolume->mutable_source()->set_type(Volume::Source::HOST_PATH);
  mountVolume->mutable_source()->mutable_host_path()->set_path(mountRootDir);
  mountVolume->mutable_source()->mutable_host_path()
    ->mutable_mount_propagation()->set_mode(MountPropagation::BIDIRECTIONAL);

  CHECK(!services.contains(containerId));
  services[containerId].reset(new Promise<csi::v0::Client>());

  Try<Owned<ContainerDaemon>> daemon = ContainerDaemon::create(
      extractParentEndpoint(url),
      authToken,
      containerId,
      commandInfo,
      config->resources(),
      containerInfo,
      std::function<Future<Nothing>()>(defer(self(), [=]() -> Future<Nothing> {
        CHECK(services.at(containerId)->associate(connect(endpointPath)));
        return services.at(containerId)->future()
          .then([] { return Nothing(); });
      })),
      std::function<Future<Nothing>()>(defer(self(), [=]() -> Future<Nothing> {
        ++metrics.csi_plugin_container_terminations;

        services.at(containerId)->discard();
        services.at(containerId).reset(new Promise<csi::v0::Client>());

        if (os::exists(endpointPath)) {
          Try<Nothing> rm = os::rm(endpointPath);
          if (rm.isError()) {
            return Failure(
                "Failed to remove endpoint '" + endpointPath + "': " +
                rm.error());
          }
        }

        return Nothing();
      })));

  if (daemon.isError()) {
    return Failure(
        "Failed to create container daemon for plugin container '" +
        stringify(containerId) + "': " + daemon.error());
  }

  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Container daemon for '" << containerId << "' failed: " << message;
    fatal();
  };

  daemons[containerId] = daemon.get();
  daemon.get()->wait()
    .onFailed(defer(self(), std::bind(die, lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, "future discarded")));

  return services.at(containerId)->future();
}


// Lists all running plugin containers for this resource provider.
// NOTE: This might return containers that are not actually running,
// e.g., if they are being destroyed.
Future<hashmap<ContainerID, Option<ContainerStatus>>>
StorageLocalResourceProviderProcess::getContainers()
{
  agent::Call call;
  call.set_type(agent::Call::GET_CONTAINERS);
  call.mutable_get_containers()->set_show_nested(false);
  call.mutable_get_containers()->set_show_standalone(true);

  return http::post(
      extractParentEndpoint(url),
      getAuthHeader(authToken) +
        http::Headers{{"Accept", stringify(contentType)}},
      serialize(contentType, evolve(call)),
      stringify(contentType))
    .then(defer(self(), [=](const http::Response& httpResponse)
        -> Future<hashmap<ContainerID, Option<ContainerStatus>>> {
      hashmap<ContainerID, Option<ContainerStatus>> result;

      if (httpResponse.status != http::OK().status) {
        return Failure(
            "Failed to get containers: Unexpected response '" +
            httpResponse.status + "' (" + httpResponse.body + ")");
      }

      Try<v1::agent::Response> v1Response =
        deserialize<v1::agent::Response>(contentType, httpResponse.body);
      if (v1Response.isError()) {
        return Failure("Failed to get containers: " + v1Response.error());
      }

      const Principal principal = LocalResourceProvider::principal(info);
      CHECK(principal.claims.contains("cid_prefix"));

      const string& cidPrefix = principal.claims.at("cid_prefix");

      agent::Response response = devolve(v1Response.get());
      foreach (const agent::Response::GetContainers::Container& container,
               response.get_containers().containers()) {
        if (strings::startsWith(container.container_id().value(), cidPrefix)) {
          result.put(
              container.container_id(),
              container.has_container_status()
                ? container.container_status()
                : Option<ContainerStatus>::none());
        }
      }

      return result;
    }));
}


// Waits for the specified plugin container to be terminated.
Future<Nothing> StorageLocalResourceProviderProcess::waitContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_CONTAINER);
  call.mutable_wait_container()->mutable_container_id()->CopyFrom(containerId);

  return http::post(
      extractParentEndpoint(url),
      getAuthHeader(authToken),
      serialize(contentType, evolve(call)),
      stringify(contentType))
    .then([containerId](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status &&
          response.status != http::NotFound().status) {
        return Failure(
            "Failed to wait for container '" + stringify(containerId) +
            "': Unexpected response '" + response.status + "' (" + response.body
            + ")");
      }

      return Nothing();
    });
}


// Kills the specified plugin container.
Future<Nothing> StorageLocalResourceProviderProcess::killContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::KILL_CONTAINER);
  call.mutable_kill_container()->mutable_container_id()->CopyFrom(containerId);

  return http::post(
      extractParentEndpoint(url),
      getAuthHeader(authToken),
      serialize(contentType, evolve(call)),
      stringify(contentType))
    .then([containerId](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status &&
          response.status != http::NotFound().status) {
        return Failure(
            "Failed to kill container '" + stringify(containerId) +
            "': Unexpected response '" + response.status + "' (" + response.body
            + ")");
      }

      return Nothing();
    });
}


Future<Nothing> StorageLocalResourceProviderProcess::prepareIdentityService()
{
  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the plugin info.
      return call<csi::v0::GET_PLUGIN_INFO>(
          client, csi::v0::GetPluginInfoRequest())
        .then(defer(self(), [=](
            const csi::v0::GetPluginInfoResponse& response) {
          pluginInfo = response;

          LOG(INFO) << "Node plugin loaded: " << stringify(pluginInfo.get());

          // Get the latest service future before proceeding to the next step.
          return getService(nodeContainerId.get());
        }));
    }))
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the plugin capabilities.
      return call<csi::v0::GET_PLUGIN_CAPABILITIES>(
          client, csi::v0::GetPluginCapabilitiesRequest())
        .then(defer(self(), [=](
            const csi::v0::GetPluginCapabilitiesResponse& response) {
          pluginCapabilities = response.capabilities();

          return Nothing();
        }));
    }));
}


// NOTE: This can only be called after `prepareIdentityService`.
Future<Nothing> StorageLocalResourceProviderProcess::prepareControllerService()
{
  CHECK_SOME(pluginInfo);

  if (!pluginCapabilities.controllerService) {
    return Nothing();
  }

  if (controllerContainerId.isNone()) {
    return Failure(
        stringify(CSIPluginContainerInfo::CONTROLLER_SERVICE) + " not found");
  }

  return getService(controllerContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the controller plugin info and check for consistency.
      return call<csi::v0::GET_PLUGIN_INFO>(
          client, csi::v0::GetPluginInfoRequest())
        .then(defer(self(), [=](
            const csi::v0::GetPluginInfoResponse& response) {
          LOG(INFO) << "Controller plugin loaded: " << stringify(response);

          if (pluginInfo->name() != response.name() ||
              pluginInfo->vendor_version() != response.vendor_version()) {
            LOG(WARNING)
              << "Inconsistent controller and node plugin components. Please "
                 "check with the plugin vendor to ensure compatibility.";
          }

          // Get the latest service future before proceeding to the next step.
          return getService(controllerContainerId.get());
        }));
    }))
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the controller capabilities.
      return call<csi::v0::CONTROLLER_GET_CAPABILITIES>(
          client, csi::v0::ControllerGetCapabilitiesRequest())
        .then(defer(self(), [=](
            const csi::v0::ControllerGetCapabilitiesResponse& response) {
          controllerCapabilities = response.capabilities();

          return Nothing();
        }));
    }));
}


// NOTE: This can only be called after `prepareIdentityService` and
// `prepareControllerService`.
Future<Nothing> StorageLocalResourceProviderProcess::prepareNodeService()
{
  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the node capabilities.
      return call<csi::v0::NODE_GET_CAPABILITIES>(
          client, csi::v0::NodeGetCapabilitiesRequest())
        .then(defer(self(), [=](
            const csi::v0::NodeGetCapabilitiesResponse& response)
            -> Future<csi::v0::Client> {
          nodeCapabilities = response.capabilities();

          // Get the latest service future before proceeding to the next step.
          return getService(nodeContainerId.get());
        }))
        .then(defer(self(), [=](csi::v0::Client client) -> Future<Nothing> {
          if (!controllerCapabilities.publishUnpublishVolume) {
            return Nothing();
          }

          // Get the node ID.
          return call<csi::v0::NODE_GET_ID>(client, csi::v0::NodeGetIdRequest())
            .then(defer(self(), [=](
                const csi::v0::NodeGetIdResponse& response) {
              nodeId = response.node_id();

              return Nothing();
            }));
        }));
    }));
}


// Transitions the state of the specified volume from `CREATED` or
// `CONTROLLER_PUBLISH` to `NODE_READY`.
// NOTE: This can only be called after `prepareControllerService` and
// `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::controllerPublish(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeData& volume = volumes.at(volumeId);

  if (!controllerCapabilities.publishUnpublishVolume) {
    CHECK_EQ(VolumeState::CREATED, volume.state.state());

    volume.state.set_state(VolumeState::NODE_READY);
    checkpointVolumeState(volumeId);

    return Nothing();
  }

  CHECK_SOME(controllerContainerId);
  CHECK_SOME(nodeId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [this, volumeId](
        csi::v0::Client client) -> Future<Nothing> {
      VolumeData& volume = volumes.at(volumeId);

      if (volume.state.state() == VolumeState::CREATED) {
        volume.state.set_state(VolumeState::CONTROLLER_PUBLISH);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::CONTROLLER_PUBLISH, volume.state.state());

      csi::v0::ControllerPublishVolumeRequest request;
      request.set_volume_id(volumeId);
      request.set_node_id(nodeId.get());
      request.mutable_volume_capability()
        ->CopyFrom(volume.state.volume_capability());
      request.set_readonly(false);
      *request.mutable_volume_attributes() = volume.state.volume_attributes();

      return call<csi::v0::CONTROLLER_PUBLISH_VOLUME>(
          client, std::move(request))
        .then(defer(self(), [this, volumeId](
            const csi::v0::ControllerPublishVolumeResponse& response) {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::NODE_READY);
          *volume.state.mutable_publish_info() = response.publish_info();
          checkpointVolumeState(volumeId);

          return Nothing();
        }));
    }));
}


// Transitions the state of the specified volume from `NODE_READY`,
// `CONTROLLER_PUBLISH` or `CONTROLLER_UNPUBLISH` to `CREATED`.
// NOTE: This can only be called after `prepareControllerService` and
// `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::controllerUnpublish(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeData& volume = volumes.at(volumeId);

  if (!controllerCapabilities.publishUnpublishVolume) {
    CHECK_EQ(VolumeState::NODE_READY, volume.state.state());

    volume.state.set_state(VolumeState::CREATED);
    checkpointVolumeState(volumeId);

    return Nothing();
  }

  CHECK_SOME(controllerContainerId);
  CHECK_SOME(nodeId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [this, volumeId](csi::v0::Client client) {
      VolumeData& volume = volumes.at(volumeId);

      // A previously failed `ControllerPublishVolume` call can be recovered
      // through the current `ControllerUnpublishVolume` call. See:
      // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#controllerpublishvolume // NOLINT
      if (volume.state.state() == VolumeState::NODE_READY ||
          volume.state.state() == VolumeState::CONTROLLER_PUBLISH) {
        volume.state.set_state(VolumeState::CONTROLLER_UNPUBLISH);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::CONTROLLER_UNPUBLISH, volume.state.state());

      csi::v0::ControllerUnpublishVolumeRequest request;
      request.set_volume_id(volumeId);
      request.set_node_id(nodeId.get());

      return call<csi::v0::CONTROLLER_UNPUBLISH_VOLUME>(
          client, std::move(request))
        .then(defer(self(), [this, volumeId] {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::CREATED);
          volume.state.mutable_publish_info()->clear();
          checkpointVolumeState(volumeId);

          return Nothing();
        }));
    }));
}


// Transitions the state of the specified volume from `NODE_READY` or
// `NODE_STAGE` to `VOL_READY`.
// NOTE: This can only be called after `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::nodeStage(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeData& volume = volumes.at(volumeId);

  if (!nodeCapabilities.stageUnstageVolume) {
    CHECK_EQ(VolumeState::NODE_READY, volume.state.state());

    volume.state.set_state(VolumeState::VOL_READY);
    volume.state.set_boot_id(bootId);
    checkpointVolumeState(volumeId);

    return Nothing();
  }

  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [this, volumeId](
        csi::v0::Client client) -> Future<Nothing> {
      VolumeData& volume = volumes.at(volumeId);

      const string stagingPath = csi::paths::getMountStagingPath(
          csi::paths::getMountRootDir(
              slave::paths::getCsiRootDir(workDir),
              info.storage().plugin().type(),
              info.storage().plugin().name()),
          volumeId);

      Try<Nothing> mkdir = os::mkdir(stagingPath);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create mount staging path '" + stagingPath + "': " +
            mkdir.error());
      }

      if (volume.state.state() == VolumeState::NODE_READY) {
        volume.state.set_state(VolumeState::NODE_STAGE);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::NODE_STAGE, volume.state.state());

      csi::v0::NodeStageVolumeRequest request;
      request.set_volume_id(volumeId);
      *request.mutable_publish_info() = volume.state.publish_info();
      request.set_staging_target_path(stagingPath);
      request.mutable_volume_capability()
        ->CopyFrom(volume.state.volume_capability());
      *request.mutable_volume_attributes() = volume.state.volume_attributes();

      return call<csi::v0::NODE_STAGE_VOLUME>(client, std::move(request))
        .then(defer(self(), [this, volumeId] {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::VOL_READY);
          volume.state.set_boot_id(bootId);
          checkpointVolumeState(volumeId);

          return Nothing();
        }));
    }));
}


// Transitions the state of the specified volume from `VOL_READY`, `NODE_STAGE`
// or `NODE_UNSTAGE` to `NODE_READY`.
// NOTE: This can only be called after `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::nodeUnstage(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeData& volume = volumes.at(volumeId);

  if (!nodeCapabilities.stageUnstageVolume) {
    CHECK_EQ(VolumeState::VOL_READY, volume.state.state());

    volume.state.set_state(VolumeState::NODE_READY);
    volume.state.clear_boot_id();
    checkpointVolumeState(volumeId);

    return Nothing();
  }

  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [this, volumeId](csi::v0::Client client) {
      VolumeData& volume = volumes.at(volumeId);

      const string stagingPath = csi::paths::getMountStagingPath(
          csi::paths::getMountRootDir(
              slave::paths::getCsiRootDir(workDir),
              info.storage().plugin().type(),
              info.storage().plugin().name()),
          volumeId);

      CHECK(os::exists(stagingPath));

      // A previously failed `NodeStageVolume` call can be recovered through the
      // current `NodeUnstageVolume` call. See:
      // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodestagevolume // NOLINT
      if (volume.state.state() == VolumeState::VOL_READY ||
          volume.state.state() == VolumeState::NODE_STAGE) {
        volume.state.set_state(VolumeState::NODE_UNSTAGE);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::NODE_UNSTAGE, volume.state.state());

      csi::v0::NodeUnstageVolumeRequest request;
      request.set_volume_id(volumeId);
      request.set_staging_target_path(stagingPath);

      return call<csi::v0::NODE_UNSTAGE_VOLUME>(client, std::move(request))
        .then(defer(self(), [this, volumeId] {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::NODE_READY);
          volume.state.clear_boot_id();
          checkpointVolumeState(volumeId);

          return Nothing();
        }));
    }));
}


// Transitions the state of the specified volume from `VOL_READY` or
// `NODE_PUBLISH` to `PUBLISHED`.
// NOTE: This can only be called after `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::nodePublish(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [this, volumeId](
        csi::v0::Client client) -> Future<Nothing> {
      VolumeData& volume = volumes.at(volumeId);

      const string targetPath = csi::paths::getMountTargetPath(
          csi::paths::getMountRootDir(
              slave::paths::getCsiRootDir(workDir),
              info.storage().plugin().type(),
              info.storage().plugin().name()),
          volumeId);

      Try<Nothing> mkdir = os::mkdir(targetPath);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create mount target path '" + targetPath + "': " +
            mkdir.error());
      }

      if (volume.state.state() == VolumeState::VOL_READY) {
        volume.state.set_state(VolumeState::NODE_PUBLISH);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::NODE_PUBLISH, volume.state.state());

      csi::v0::NodePublishVolumeRequest request;
      request.set_volume_id(volumeId);
      *request.mutable_publish_info() = volume.state.publish_info();
      request.set_target_path(targetPath);
      request.mutable_volume_capability()
        ->CopyFrom(volume.state.volume_capability());
      request.set_readonly(false);
      *request.mutable_volume_attributes() = volume.state.volume_attributes();

      if (nodeCapabilities.stageUnstageVolume) {
        const string stagingPath = csi::paths::getMountStagingPath(
            csi::paths::getMountRootDir(
                slave::paths::getCsiRootDir(workDir),
                info.storage().plugin().type(),
                info.storage().plugin().name()),
            volumeId);

        CHECK(os::exists(stagingPath));

        request.set_staging_target_path(stagingPath);
      }

      return call<csi::v0::NODE_PUBLISH_VOLUME>(client, std::move(request))
        .then(defer(self(), [this, volumeId] {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::PUBLISHED);
          checkpointVolumeState(volumeId);

          return Nothing();
        }));
    }));
}


// Transitions the state of the specified volume from `PUBLISHED`,
// `NODE_PUBLISH` or `NODE_UNPUBLISH` to `VOL_READY`.
// NOTE: This can only be called after `prepareNodeService`.
Future<Nothing> StorageLocalResourceProviderProcess::nodeUnpublish(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [this, volumeId](csi::v0::Client client) {
      VolumeData& volume = volumes.at(volumeId);

      const string targetPath = csi::paths::getMountTargetPath(
          csi::paths::getMountRootDir(
              slave::paths::getCsiRootDir(workDir),
              info.storage().plugin().type(),
              info.storage().plugin().name()),
          volumeId);

      CHECK(os::exists(targetPath));

      // A previously failed `NodePublishVolume` call can be recovered through
      // the current `NodeUnpublishVolume` call. See:
      // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodepublishvolume // NOLINT
      if (volume.state.state() == VolumeState::PUBLISHED ||
          volume.state.state() == VolumeState::NODE_PUBLISH) {
        volume.state.set_state(VolumeState::NODE_UNPUBLISH);
        checkpointVolumeState(volumeId);
      }

      CHECK_EQ(VolumeState::NODE_UNPUBLISH, volume.state.state());

      csi::v0::NodeUnpublishVolumeRequest request;
      request.set_volume_id(volumeId);
      request.set_target_path(targetPath);

      return call<csi::v0::NODE_UNPUBLISH_VOLUME>(client, std::move(request))
        .then(defer(self(), [this, volumeId, targetPath]() -> Future<Nothing> {
          VolumeData& volume = volumes.at(volumeId);

          volume.state.set_state(VolumeState::VOL_READY);
          checkpointVolumeState(volumeId);

          Try<Nothing> rmdir = os::rmdir(targetPath);
          if (rmdir.isError()) {
            return Failure(
                "Failed to remove mount point '" + targetPath + "': " +
                rmdir.error());
          }

          return Nothing();
        }));
    }));
}


// Returns a CSI volume ID.
// NOTE: This can only be called after `prepareControllerService`.
Future<string> StorageLocalResourceProviderProcess::createVolume(
    const string& name,
    const Bytes& capacity,
    const DiskProfileAdaptor::ProfileInfo& profileInfo)
{
  if (!controllerCapabilities.createDeleteVolume) {
    return Failure(
        "Controller capability 'CREATE_DELETE_VOLUME' is not supported");
  }

  CHECK_SOME(controllerContainerId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      csi::v0::CreateVolumeRequest request;
      request.set_name(name);
      request.mutable_capacity_range()
        ->set_required_bytes(capacity.bytes());
      request.mutable_capacity_range()
        ->set_limit_bytes(capacity.bytes());
      request.add_volume_capabilities()->CopyFrom(profileInfo.capability);
      *request.mutable_parameters() = profileInfo.parameters;

      return call<csi::v0::CREATE_VOLUME>(client, std::move(request))
        .then(defer(self(), [=](
            const csi::v0::CreateVolumeResponse& response) -> string {
          const csi::v0::Volume& volume = response.volume();

          if (volumes.contains(volume.id())) {
            // The resource provider failed over after the last
            // `CreateVolume` call, but before the operation status was
            // checkpointed.
            CHECK_EQ(VolumeState::CREATED,
                     volumes.at(volume.id()).state.state());
          } else {
            VolumeState volumeState;
            volumeState.set_state(VolumeState::CREATED);
            volumeState.mutable_volume_capability()
              ->CopyFrom(profileInfo.capability);
            *volumeState.mutable_volume_attributes() = volume.attributes();

            volumes.put(volume.id(), std::move(volumeState));
            checkpointVolumeState(volume.id());
          }

          return volume.id();
        }));
    }));
}


// NOTE: This can only be called after `prepareControllerService` and
// `prepareNodeService` (since it may require `NodeUnpublishVolume`).
Future<Nothing> StorageLocalResourceProviderProcess::deleteVolume(
    const string& volumeId,
    bool preExisting)
{
  // We do not need the capability for pre-existing volumes since no
  // actual `DeleteVolume` call will be made.
  if (!preExisting && !controllerCapabilities.createDeleteVolume) {
    return Failure(
        "Controller capability 'CREATE_DELETE_VOLUME' is not supported");
  }

  CHECK_SOME(controllerContainerId);

  const string volumePath = csi::paths::getVolumePath(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name(),
      volumeId);

  if (!volumes.contains(volumeId)) {
    // The resource provider failed over after the last `DeleteVolume`
    // call, but before the operation status was checkpointed.
    CHECK(!os::exists(volumePath));

    return Nothing();
  }

  const VolumeData& volume = volumes.at(volumeId);

  Future<Nothing> deleted = Nothing();

  CHECK(VolumeState::State_IsValid(volume.state.state()));

  switch (volume.state.state()) {
    case VolumeState::PUBLISHED:
    case VolumeState::NODE_PUBLISH:
    case VolumeState::NODE_UNPUBLISH: {
      deleted = deleted
        .then(defer(self(), &Self::nodeUnpublish, volumeId));

      // NOTE: We continue to the next case to delete the volume in `VOL_READY`
      // state once the above is done.
    }
    case VolumeState::VOL_READY:
    case VolumeState::NODE_STAGE:
    case VolumeState::NODE_UNSTAGE: {
      deleted = deleted
        .then(defer(self(), &Self::nodeUnstage, volumeId));

      // NOTE: We continue to the next case to delete the volume in `NODE_READY`
      // state once the above is done.
    }
    case VolumeState::NODE_READY:
    case VolumeState::CONTROLLER_PUBLISH:
    case VolumeState::CONTROLLER_UNPUBLISH: {
      deleted = deleted
        .then(defer(self(), &Self::controllerUnpublish, volumeId));

      // NOTE: We continue to the next case to delete the volume in `CREATED`
      // state once the above is done.
    }
    case VolumeState::CREATED: {
      if (!preExisting) {
        deleted = deleted
          .then(defer(self(), &Self::getService, controllerContainerId.get()))
          .then(defer(self(), [this, volumeId](csi::v0::Client client) {
            csi::v0::DeleteVolumeRequest request;
            request.set_volume_id(volumeId);

            return call<csi::v0::DELETE_VOLUME>(client, std::move(request))
              .then([] { return Nothing(); });
          }));
      }

      break;
    }
    case VolumeState::UNKNOWN: {
      UNREACHABLE();
    }

    // NOTE: We avoid using a default clause for the following values in
    // proto3's open enum to enable the compiler to detect missing enum cases
    // for us. See:
    // https://github.com/google/protobuf/issues/3917
    case google::protobuf::kint32min:
    case google::protobuf::kint32max: {
      UNREACHABLE();
    }
  }

  // NOTE: The last asynchronous continuation of `deleteVolume`, which is
  // supposed to be run in the volume's sequence, would cause the sequence to be
  // destructed, which would in turn discard the returned future. However, since
  // the continuation would have already been run, the returned future will
  // become ready, making the future returned by the sequence ready as well.
  return deleted
    .then(defer(self(), [this, volumeId, volumePath] {
      volumes.erase(volumeId);
      CHECK_SOME(os::rmdir(volumePath));

      return Nothing();
    }));
}


// Validates if a volume has the specified capability. This is called when
// applying `CREATE_DISK` on a pre-existing volume, so we make it return a
// volume ID, similar to `createVolume`.
// NOTE: This can only be called after `prepareIdentityService` and only for
// newly discovered volumes.
Future<string> StorageLocalResourceProviderProcess::validateCapability(
    const string& volumeId,
    const Option<Labels>& metadata,
    const csi::v0::VolumeCapability& capability)
{
  CHECK(!volumes.contains(volumeId));

  if (!pluginCapabilities.controllerService) {
    return Failure(
        "Plugin capability 'CONTROLLER_SERVICE' is not supported");
  }

  CHECK_SOME(controllerContainerId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      google::protobuf::Map<string, string> volumeAttributes;

      if (metadata.isSome()) {
        volumeAttributes = convertLabelsToStringMap(metadata.get()).get();
      }

      csi::v0::ValidateVolumeCapabilitiesRequest request;
      request.set_volume_id(volumeId);
      request.add_volume_capabilities()->CopyFrom(capability);
      *request.mutable_volume_attributes() = volumeAttributes;

      return call<csi::v0::VALIDATE_VOLUME_CAPABILITIES>(
          client, std::move(request))
        .then(defer(self(), [=](
            const csi::v0::ValidateVolumeCapabilitiesResponse& response)
            -> Future<string> {
          if (!response.supported()) {
            return Failure(
                "Unsupported volume capability for volume '" + volumeId +
                "': " + response.message());
          }

          VolumeState volumeState;
          volumeState.set_state(VolumeState::CREATED);
          volumeState.mutable_volume_capability()->CopyFrom(capability);
          *volumeState.mutable_volume_attributes() = volumeAttributes;

          volumes.put(volumeId, std::move(volumeState));
          checkpointVolumeState(volumeId);

          return volumeId;
        }));
    }));
}


// NOTE: This can only be called after `prepareControllerService` and
// the resource provider ID has been obtained.
Future<Resources> StorageLocalResourceProviderProcess::listVolumes()
{
  CHECK(info.has_id());

  // This is only used for reconciliation so no failure is returned.
  if (!controllerCapabilities.listVolumes) {
    return Resources();
  }

  CHECK_SOME(controllerContainerId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      // TODO(chhsiao): Set the max entries and use a loop to do
      // multiple `ListVolumes` calls.
      return call<csi::v0::LIST_VOLUMES>(client, csi::v0::ListVolumesRequest())
        .then(defer(self(), [=](const csi::v0::ListVolumesResponse& response) {
          Resources resources;

          // Recover disk profiles from the checkpointed state.
          hashmap<string, string> volumesToProfiles;
          foreach (const Resource& resource, totalResources) {
            if (resource.disk().source().has_id() &&
                resource.disk().source().has_profile()) {
              volumesToProfiles.put(
                  resource.disk().source().id(),
                  resource.disk().source().profile());
            }
          }

          foreach (const auto& entry, response.entries()) {
            resources += createRawDiskResource(
                info,
                Bytes(entry.volume().capacity_bytes()),
                volumesToProfiles.contains(entry.volume().id())
                  ? volumesToProfiles.at(entry.volume().id())
                  : Option<string>::none(),
                entry.volume().id(),
                entry.volume().attributes().empty()
                  ? Option<Labels>::none()
                  : convertStringMapToLabels(entry.volume().attributes()));
          }

          return resources;
        }));
    }));
}


// NOTE: This can only be called after `prepareControllerService` and
// the resource provider ID has been obtained.
Future<Resources> StorageLocalResourceProviderProcess::getCapacities()
{
  CHECK(info.has_id());

  // This is only used for reconciliation so no failure is returned.
  if (!controllerCapabilities.getCapacity) {
    return Resources();
  }

  CHECK_SOME(controllerContainerId);

  return getService(controllerContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      vector<Future<Resources>> futures;

      foreachpair (const string& profile,
                   const DiskProfileAdaptor::ProfileInfo& profileInfo,
                   profileInfos) {
        csi::v0::GetCapacityRequest request;
        request.add_volume_capabilities()->CopyFrom(profileInfo.capability);
        *request.mutable_parameters() = profileInfo.parameters;

        futures.push_back(call<csi::v0::GET_CAPACITY>(
            client, std::move(request))
          .then(defer(self(), [=](
              const csi::v0::GetCapacityResponse& response) -> Resources {
            if (response.available_capacity() == 0) {
              return Resources();
            }

            return createRawDiskResource(
                info,
                Bytes(response.available_capacity()),
                profile);
          })));
      }

      return collect(futures)
        .then([](const vector<Resources>& resources) {
          return accumulate(resources.begin(), resources.end(), Resources());
        });
    }));
}


// Applies the operation. Speculative operations will be synchronously
// applied. Do nothing if the operation is already in a terminal state.
Future<Nothing> StorageLocalResourceProviderProcess::_applyOperation(
    const id::UUID& operationUuid)
{
  CHECK(operations.contains(operationUuid));
  const Operation& operation = operations.at(operationUuid);

  CHECK(!protobuf::isTerminalState(operation.latest_status().state()));

  Future<vector<ResourceConversion>> conversions;

  switch (operation.info().type()) {
    case Offer::Operation::RESERVE:
    case Offer::Operation::UNRESERVE:
    case Offer::Operation::CREATE:
    case Offer::Operation::DESTROY: {
      // Synchronously apply the speculative operations to ensure that
      // its result is reflected in the total resources before any of
      // its succeeding operations is applied.
      return updateOperationStatus(
          operationUuid,
          getResourceConversions(operation.info()));
    }
    case Offer::Operation::CREATE_DISK: {
      CHECK(operation.info().has_create_disk());

      conversions = applyCreateDisk(
          operation.info().create_disk().source(),
          operationUuid,
          operation.info().create_disk().target_type());

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


// Sends `OPERATION_DROPPED` without checkpointing the status of
// the operation.
void StorageLocalResourceProviderProcess::dropOperation(
    const id::UUID& operationUuid,
    const Option<FrameworkID>& frameworkId,
    const Option<Offer::Operation>& operation,
    const string& message)
{
  LOG(WARNING)
    << "Dropping operation (uuid: " << operationUuid << "): " << message;

  UpdateOperationStatusMessage update =
    protobuf::createUpdateOperationStatusMessage(
       protobuf::createUUID(operationUuid),
       protobuf::createOperationStatus(
           OPERATION_DROPPED,
           operation.isSome() && operation->has_id()
             ? operation->id() : Option<OperationID>::none(),
           message,
           None(),
           id::UUID::random()),
       None(),
       frameworkId,
       slaveId);

  auto die = [=](const string& message) {
    LOG(ERROR)
      << "Failed to update status of operation (uuid: " << operationUuid
      << "): " << message;
    fatal();
  };

  statusUpdateManager.update(std::move(update), false)
    .onFailed(defer(self(), std::bind(die, lambda::_1)))
    .onDiscarded(defer(self(), std::bind(die, "future discarded")));

  ++metrics.operations_dropped.at(
      operation.isSome() ? operation->type() : Offer::Operation::UNKNOWN);
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyCreateDisk(
    const Resource& resource,
    const id::UUID& operationUuid,
    const Resource::DiskInfo::Source::Type& type)
{
  CHECK_EQ(Resource::DiskInfo::Source::RAW, resource.disk().source().type());

  // NOTE: Currently we only support two type of RAW disk resources:
  //   1. RAW disk from `GetCapacity` with a profile but no volume ID.
  //   2. RAW disk from `ListVolumes` for a pre-existing volume, which
  //      has a volume ID but no profile.
  //
  // For 1, we check if its profile is mount or block capable, then
  // call `CreateVolume` with the operation UUID as the name (so that
  // the same volume will be returned when recovering from a failover).
  //
  // For 2, there are two scenarios:
  //   a. If the volume has a checkpointed state (because it was created
  //      by a previous resource provider), we simply check if its
  //      checkpointed capability supports the conversion.
  //   b. If the volume is newly discovered, `ValidateVolumeCapabilities`
  //      is called with a default mount or block capability.
  CHECK_NE(resource.disk().source().has_profile(),
           resource.disk().source().has_id());

  Future<string> created;

  switch (type) {
    case Resource::DiskInfo::Source::MOUNT: {
      if (resource.disk().source().has_profile()) {
        // The profile exists since any operation with a stale profile must have
        // been dropped for a mismatched resource version or a reconciliation.
        CHECK(profileInfos.contains(resource.disk().source().profile()))
          << "Profile '" << resource.disk().source().profile() << "' not found";

        const DiskProfileAdaptor::ProfileInfo& profileInfo =
          profileInfos.at(resource.disk().source().profile());

        if (!profileInfo.capability.has_mount()) {
          return Failure(
              "Profile '" + resource.disk().source().profile() +
              "' cannot be used to create a MOUNT disk");
        }

        // TODO(chhsiao): Call `CreateVolume` sequentially with other
        // create or delete operations, and send an `UPDATE_STATE` for
        // RAW profiled resources afterward.
        created = createVolume(
            operationUuid.toString(),
            Bytes(resource.scalar().value() * Bytes::MEGABYTES),
            profileInfo);
      } else {
        const string& volumeId = resource.disk().source().id();

        if (volumes.contains(volumeId)) {
          if (!volumes.at(volumeId).state.volume_capability().has_mount()) {
            return Failure(
                "Volume '" + volumeId +
                "' cannot be converted to a MOUNT disk");
          }

          created = volumeId;
        } else {
          // No need to call `ValidateVolumeCapabilities` sequentially
          // since the volume is not used and thus not in `volumes` yet.
          created = validateCapability(
              volumeId,
              resource.disk().source().has_metadata()
                ? resource.disk().source().metadata() : Option<Labels>::none(),
              defaultMountCapability);
        }
      }
      break;
    }
    case Resource::DiskInfo::Source::BLOCK: {
      if (resource.disk().source().has_profile()) {
        // The profile exists since any operation with a stale profile must have
        // been dropped for a mismatched resource version or a reconciliation.
        CHECK(profileInfos.contains(resource.disk().source().profile()))
          << "Profile '" << resource.disk().source().profile() << "' not found";

        const DiskProfileAdaptor::ProfileInfo& profileInfo =
          profileInfos.at(resource.disk().source().profile());

        if (!profileInfo.capability.has_block()) {
          return Failure(
              "Profile '" + resource.disk().source().profile() +
              "' cannot be used to create a BLOCK disk");
        }

        // TODO(chhsiao): Call `CreateVolume` sequentially with other
        // create or delete operations, and send an `UPDATE_STATE` for
        // RAW profiled resources afterward.
        created = createVolume(
            operationUuid.toString(),
            Bytes(resource.scalar().value() * Bytes::MEGABYTES),
            profileInfo);
      } else {
        const string& volumeId = resource.disk().source().id();

        if (volumes.contains(volumeId)) {
          if (!volumes.at(volumeId).state.volume_capability().has_block()) {
            return Failure(
                "Volume '" + volumeId +
                "' cannot be converted to a BLOCK disk");
          }

          created = volumeId;
        } else {
          // No need to call `ValidateVolumeCapabilities` sequentially
          // since the volume is not used and thus not in `volumes` yet.
          created = validateCapability(
              volumeId,
              resource.disk().source().has_metadata()
                ? resource.disk().source().metadata() : Option<Labels>::none(),
              defaultBlockCapability);
        }
      }
      break;
    }
    case Resource::DiskInfo::Source::UNKNOWN:
    case Resource::DiskInfo::Source::PATH:
    case Resource::DiskInfo::Source::RAW: {
      UNREACHABLE();
    }
  }

  return created
    .then(defer(self(), [=](const string& volumeId) {
      CHECK(volumes.contains(volumeId));
      const VolumeState& volumeState = volumes.at(volumeId).state;

      Resource converted = resource;
      converted.mutable_disk()->mutable_source()->set_id(volumeId);
      converted.mutable_disk()->mutable_source()->set_type(type);

      if (!volumeState.volume_attributes().empty()) {
        converted.mutable_disk()->mutable_source()->mutable_metadata()
          ->CopyFrom(convertStringMapToLabels(volumeState.volume_attributes()));
      }

      const string mountRootDir = csi::paths::getMountRootDir(
          slave::paths::getCsiRootDir("."),
          info.storage().plugin().type(),
          info.storage().plugin().name());

      switch (type) {
        case Resource::DiskInfo::Source::MOUNT: {
          // Set the root path relative to agent work dir.
          converted.mutable_disk()->mutable_source()->mutable_mount()
            ->set_root(mountRootDir);

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
  CHECK(resource.disk().source().type() == Resource::DiskInfo::Source::MOUNT ||
        resource.disk().source().type() == Resource::DiskInfo::Source::BLOCK);
  CHECK(resource.disk().source().has_id());
  CHECK(volumes.contains(resource.disk().source().id()));

  // Sequentialize the deletion with other operation on the same volume.
  return volumes.at(resource.disk().source().id()).sequence->add(
      std::function<Future<Nothing>()>(defer(
          self(),
          &Self::deleteVolume,
          resource.disk().source().id(),
          !resource.disk().source().has_profile())))
    .then(defer(self(), [=]() {
      Resource converted = resource;
      converted.mutable_disk()->mutable_source()->set_type(
          Resource::DiskInfo::Source::RAW);
      converted.mutable_disk()->mutable_source()->clear_mount();

      // We only clear the volume ID and metadata if the destroyed volume is not
      // a pre-existing volume.
      if (resource.disk().source().has_profile()) {
        converted.mutable_disk()->mutable_source()->clear_id();
        converted.mutable_disk()->mutable_source()->clear_metadata();

        if (!profileInfos.contains(resource.disk().source().profile())) {
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
              << " after the disk with profile '"
              << resource.disk().source().profile() << "' has been freed";

            // Reconcile the storage pools in `sequence` to wait for any other
            // pending operation that disallow reconciliation to finish, and set
            // up `reconciled` to drop incoming operations that disallow
            // reconciliation until the storage pools are reconciled.
            reconciled = sequence.add(std::function<Future<Nothing>()>(
                defer(self(), &Self::reconcileStoragePools)));
          }
        }
      }

      vector<ResourceConversion> conversions;
      conversions.emplace_back(resource, std::move(converted));

      return conversions;
    }));
}


// Synchronously updates `totalResources` and the operation status and
// then asks the status update manager to send status updates.
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

  operation.mutable_latest_status()->CopyFrom(
      protobuf::createOperationStatus(
          error.isNone() ? OPERATION_FINISHED : OPERATION_FAILED,
          operation.info().has_id()
            ? operation.info().id() : Option<OperationID>::none(),
          error.isNone() ? Option<string>::none() : error->message,
          error.isNone() ? convertedResources : Option<Resources>::none(),
          id::UUID::random()));

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

  // NOTE: We check if the path exists since we do not checkpoint some status
  // updates, such as OPERATION_DROPPED.
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


void StorageLocalResourceProviderProcess::checkpointVolumeState(
    const string& volumeId)
{
  const string statePath = csi::paths::getVolumeStatePath(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name(),
      volumeId);

  // NOTE: We ensure the checkpoint is synced to the filesystem to avoid
  // resulting in a stale or empty checkpoint when a system crash happens.
  Try<Nothing> checkpoint =
    slave::state::checkpoint(statePath, volumes.at(volumeId).state, true);

  CHECK_SOME(checkpoint)
    << "Failed to checkpoint volume state to '" << statePath << "':"
    << checkpoint.error();
}


void StorageLocalResourceProviderProcess::sendResourceProviderStateUpdate()
{
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

  // The latest status should have been set by the status update manager.
  CHECK(_update.has_latest_status());
  update->mutable_latest_status()->CopyFrom(_update.latest_status());

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
  : csi_plugin_container_terminations(
        prefix + "csi_plugin/container_terminations")
{
  process::metrics::add(csi_plugin_container_terminations);

  vector<csi::v0::RPC> rpcs;

  // NOTE: We use a switch statement here as a compile-time sanity check so we
  // won't forget to add metrics for new RPCs in the future.
  csi::v0::RPC firstRpc = csi::v0::GET_PLUGIN_INFO;
  switch (firstRpc) {
    case csi::v0::GET_PLUGIN_INFO:
      rpcs.push_back(csi::v0::GET_PLUGIN_INFO);
    case csi::v0::GET_PLUGIN_CAPABILITIES:
      rpcs.push_back(csi::v0::GET_PLUGIN_CAPABILITIES);
    case csi::v0::PROBE:
      rpcs.push_back(csi::v0::PROBE);
    case csi::v0::CREATE_VOLUME:
      rpcs.push_back(csi::v0::CREATE_VOLUME);
    case csi::v0::DELETE_VOLUME:
      rpcs.push_back(csi::v0::DELETE_VOLUME);
    case csi::v0::CONTROLLER_PUBLISH_VOLUME:
      rpcs.push_back(csi::v0::CONTROLLER_PUBLISH_VOLUME);
    case csi::v0::CONTROLLER_UNPUBLISH_VOLUME:
      rpcs.push_back(csi::v0::CONTROLLER_UNPUBLISH_VOLUME);
    case csi::v0::VALIDATE_VOLUME_CAPABILITIES:
      rpcs.push_back(csi::v0::VALIDATE_VOLUME_CAPABILITIES);
    case csi::v0::LIST_VOLUMES:
      rpcs.push_back(csi::v0::LIST_VOLUMES);
    case csi::v0::GET_CAPACITY:
      rpcs.push_back(csi::v0::GET_CAPACITY);
    case csi::v0::CONTROLLER_GET_CAPABILITIES:
      rpcs.push_back(csi::v0::CONTROLLER_GET_CAPABILITIES);
    case csi::v0::NODE_STAGE_VOLUME:
      rpcs.push_back(csi::v0::NODE_STAGE_VOLUME);
    case csi::v0::NODE_UNSTAGE_VOLUME:
      rpcs.push_back(csi::v0::NODE_UNSTAGE_VOLUME);
    case csi::v0::NODE_PUBLISH_VOLUME:
      rpcs.push_back(csi::v0::NODE_PUBLISH_VOLUME);
    case csi::v0::NODE_UNPUBLISH_VOLUME:
      rpcs.push_back(csi::v0::NODE_UNPUBLISH_VOLUME);
    case csi::v0::NODE_GET_ID:
      rpcs.push_back(csi::v0::NODE_GET_ID);
    case csi::v0::NODE_GET_CAPABILITIES:
      rpcs.push_back(csi::v0::NODE_GET_CAPABILITIES);
  }

  foreach (const csi::v0::RPC& rpc, rpcs) {
    const string name = stringify(rpc);

    csi_plugin_rpcs_pending.put(
        rpc, PushGauge(prefix + "csi_plugin/rpcs/" + name + "/pending"));
    csi_plugin_rpcs_successes.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/successes"));
    csi_plugin_rpcs_errors.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/errors"));
    csi_plugin_rpcs_cancelled.put(
        rpc, Counter(prefix + "csi_plugin/rpcs/" + name + "/cancelled"));

    process::metrics::add(csi_plugin_rpcs_pending.at(rpc));
    process::metrics::add(csi_plugin_rpcs_successes.at(rpc));
    process::metrics::add(csi_plugin_rpcs_errors.at(rpc));
    process::metrics::add(csi_plugin_rpcs_cancelled.at(rpc));
  }

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
  process::metrics::remove(csi_plugin_container_terminations);

  foreachvalue (const PushGauge& gauge, csi_plugin_rpcs_pending) {
    process::metrics::remove(gauge);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_successes) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_errors) {
    process::metrics::remove(counter);
  }

  foreachvalue (const Counter& counter, csi_plugin_rpcs_cancelled) {
    process::metrics::remove(counter);
  }

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
            CSIPluginContainerInfo::NODE_SERVICE)) {
      hasNodeService = true;
      break;
    }
  }

  if (!hasNodeService) {
    return Error(
        stringify(CSIPluginContainerInfo::NODE_SERVICE) + " not found");
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
