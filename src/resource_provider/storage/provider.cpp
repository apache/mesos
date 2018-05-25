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

#include <glog/logging.h>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>
#include <process/timeout.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/resource_provider/resource_provider.hpp>
#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "csi/client.hpp"
#include "csi/paths.hpp"
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

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;
using process::Sequence;
using process::Timeout;

using process::after;
using process::collect;
using process::defer;
using process::loop;
using process::spawn;

using process::http::authentication::Principal;

using process::metrics::Counter;

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


// Returns a prefix for naming standalone containers to run CSI plugins
// for the resource provider. The prefix is of the following format:
//     <rp_type>-<rp_name>--
// where <rp_type> and <rp_name> are the type and name of the resource
// provider, with dots replaced by dashes. We use a double-dash at the
// end to explicitly mark the end of the prefix.
static inline string getContainerIdPrefix(const ResourceProviderInfo& info)
{
  return strings::join(
      "-",
      strings::replace(info.type(), ".", "-"),
      info.name(),
      "-");
}


// Returns the container ID of the standalone container to run a CSI
// plugin component. The container ID is of the following format:
//     <rp_type>-<rp_name>--<csi_type>-<csi_name>--<list_of_services>
// where <rp_type> and <rp_name> are the type and name of the resource
// provider, and <csi_type> and <csi_name> are the type and name of the
// CSI plugin, with dots replaced by dashes. <list_of_services> lists
// the CSI services provided by the component, concatenated with dashes.
static inline ContainerID getContainerId(
    const ResourceProviderInfo& info,
    const CSIPluginContainerInfo& container)
{
  string value = getContainerIdPrefix(info);

  value += strings::join(
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
      reconciling(false),
      resourceVersion(id::UUID::random()),
      operationSequence("operation-sequence"),
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

  // Helper for updating the profiles mapping upon receiving an updated
  // set of profiles from the DiskProfileAdaptor module.
  Future<Nothing> updateProfiles();

  // Reconcile the storage pools upon profile updates.
  Future<Nothing> reconcileProfileUpdates();

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void applyOperation(const Event::ApplyOperation& operation);
  void publishResources(const Event::PublishResources& publish);
  void acknowledgeOperationStatus(
      const Event::AcknowledgeOperationStatus& acknowledge);
  void reconcileOperations(
      const Event::ReconcileOperations& reconcile);

  Future<csi::v0::Client> connect(const string& endpoint);
  Future<csi::v0::Client> getService(const ContainerID& containerId);
  Future<Nothing> killService(const ContainerID& containerId);

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
      const Option<OperationID>& operationId,
      const string& message);

  Future<vector<ResourceConversion>> applyCreateVolumeOrBlock(
      const Resource& resource,
      const id::UUID& operationUuid,
      const Resource::DiskInfo::Source::Type& type);
  Future<vector<ResourceConversion>> applyDestroyVolumeOrBlock(
      const Resource& resource);

  Try<Nothing> updateOperationStatus(
      const id::UUID& operationUuid,
      const Try<vector<ResourceConversion>>& conversions);

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

  // The last set of profile names fetched from the DiskProfileAdaptor.
  hashset<string> knownProfiles;

  // True if a reconciliation of storage pools is happening.
  bool reconciling;

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

  // We maintain a sequence to keep track of ongoing volume/block
  // creation or destroy. These operations will not be sequentialized
  // through the sequence. It is simply used to wait for them to finish.
  Sequence operationSequence;

  struct Metrics
  {
    explicit Metrics(const string& prefix);
    ~Metrics();

    Counter csi_controller_plugin_terminations;
    Counter csi_node_plugin_terminations;
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
        << "' and name '" << info.name();

      state = DISCONNECTED;

      statusUpdateManager.pause();

      auto err = [](const string& message) {
        LOG(ERROR)
          << "Failed to watch for DiskProfileAdaptor: " << message;
      };

      // Start watching the DiskProfileAdaptor.
      // TODO(chhsiao): Consider retrying with backoff.
      loop(
          self(),
          [=] {
            return diskProfileAdaptor->watch(knownProfiles, info)
              .then(defer(self(), [=](const hashset<string>& profiles) {
                // Save the returned set of profiles so that we
                // can watch the module for changes to it.
                knownProfiles = profiles;

                return updateProfiles()
                  .then(defer(self(), &Self::reconcileProfileUpdates));
              }));
          },
          [](Nothing) -> ControlFlow<Nothing> { return Continue(); })
        .onFailed(std::bind(err, lambda::_1))
        .onDiscarded(std::bind(err, "future discarded"));

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
  Try<list<string>> containerPaths = csi::paths::getContainerPaths(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name());

  if (containerPaths.isError()) {
    return Failure(
        "Failed to find plugin containers for CSI plugin type '" +
        info.storage().plugin().type() + "' and name '" +
        info.storage().plugin().name() + ": " +
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

    CHECK_SOME(nodeContainerId);

    // Do not kill the up-to-date controller or node container.
    // Otherwise, kill them and perform cleanups.
    if (nodeContainerId == containerId ||
        controllerContainerId == containerId) {
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
              "Failed to read plugin container config from '" +
              configPath + "': " + config.error());
        }

        if (config.isSome() &&
            getCSIPluginContainerInfo(info, containerId) == config.get()) {
          continue;
        }
      }
    }

    futures.push_back(killService(containerId)
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
                "Failed to remove endpoint directory '" + endpointDir.get() +
                "': " + rmdir.error());
          }
        }

        Try<Nothing> rmdir = os::rmdir(path);
        if (rmdir.isError()) {
          return Failure(
              "Failed to remove plugin container directory '" + path + "': " +
              rmdir.error());
        }

        return Nothing();
      })));
  }

  // NOTE: The `Controller` service is supported if the plugin has the
  // `CONTROLLER_SERVICE` capability, and the `NodeGetId` call is
  // supported if the `Controller` service has the
  // `PUBLISH_UNPUBLISH_VOLUME` capability. Therefore, we first launch
  // the node plugin to get the plugin capabilities, then decide if we
  // need to launch the controller plugin and get the node ID.
  return collect(futures)
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
        info.storage().plugin().name() + ": " + volumePaths.error());
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
        "Failed to read the latest symlink for resource provider with "
        "type '" + info.type() + "' and name '" + info.name() + "'"
        ": " + realpath.error());
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

    CHECK(operations.contains(uuid.get()));
    operationUuids.emplace_back(std::move(uuid.get()));
  }

  return statusUpdateManager.recover(operationUuids, strict)
    .then(defer(self(), [=](
        const OperationStatusUpdateManagerState& statusUpdateManagerState)
        -> Future<Nothing> {
      using StreamState =
        typename OperationStatusUpdateManagerState::StreamState;

      // Clean up the operations that are terminated.
      foreachpair (const id::UUID& uuid,
                   const Option<StreamState>& stream,
                   statusUpdateManagerState.streams) {
        if (stream.isSome() && stream->terminated) {
          operations.erase(uuid);

          // Garbage collect the operation metadata.
          const string path = slave::paths::getOperationPath(
              slave::paths::getResourceProviderPath(
                  metaDir, slaveId, info.type(), info.name(), info.id()),
              uuid);

          Try<Nothing> rmdir = os::rmdir(path);
          if (rmdir.isError()) {
            return Failure(
                "Failed to remove directory '" + path + "': " + rmdir.error());
          }
        }
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
        if (protobuf::isTerminalState(operation.latest_status().state())) {
          continue;
        }

        auto err = [](const id::UUID& uuid, const string& message) {
          LOG(ERROR)
            << "Falied to apply operation (uuid: " << uuid << "): "
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


Future<Nothing> StorageLocalResourceProviderProcess::updateProfiles()
{
  LOG(INFO)
    << "Updating metadata for profiles: " << stringify(knownProfiles);

  vector<Future<Nothing>> futures;
  foreach (const string& profile, knownProfiles) {
    // Since profiles are immutable after creation and cannot be
    // deleted, we do not need to update any profile that is already in
    // the mapping.
    // TODO(chhsiao): Handle profile deactivation.
    if (profileInfos.contains(profile)) {
      continue;
    }

    futures.push_back(diskProfileAdaptor->translate(profile, info)
      .then(defer(self(), [=](const DiskProfileAdaptor::ProfileInfo& info) {
        profileInfos.put(profile, info);
        return Nothing();
      })));
  }

  return collect(futures).then([] { return Nothing(); });
}


Future<Nothing> StorageLocalResourceProviderProcess::reconcileProfileUpdates()
{
  // Do nothing if the resource provider ID is not known yet, since it
  // is used to construct the resource metadata of storage pools. The
  // metadata will be constructed in `reconcileResourceProviderState`.
  if (!info.has_id()) {
    return Nothing();
  }

  CHECK(!reconciling);

  LOG(INFO) << "Reconciling storage pools for resource provider " << info.id();

  reconciling = true;

  // We add a lambda into `OperationSequence` so that it will return
  // after waiting for all pending operations in the sequence.
  return operationSequence.add(
      std::function<Future<Nothing>()>([] { return Nothing(); }))
    .then(defer(self(), &Self::getCapacities))
    .then(defer(self(), [=](const Resources& discovered) {
      auto isStoragePool = [](const Resource& r) {
        return !r.disk().source().has_id();
      };

      ResourceConversion conversion = reconcileResources(
          totalResources.filter(isStoragePool),
          discovered);

      Try<Resources> result = totalResources.apply(conversion);
      CHECK_SOME(result);

      if (result.get() != totalResources) {
        LOG(INFO)
          << "Removing '" << conversion.consumed << "' and adding '"
          << conversion.converted << "' to the total resources";

        totalResources = result.get();
        checkpointResourceProviderState();

        // NOTE: We ensure that the first `UPDATE_STATE` of the current
        // subscription is sent by `reconcileResourceProviderState`, so
        // that the total resources contain existing volumes.
        if (state == READY) {
          // NOTE: We always update the resource version before sending
          // an `UPDATE_STATE`, so that any racing speculative operation
          // will be rejected. Otherwise, the speculative resource
          // conversion done on the master will be cancelled out.
          resourceVersion = id::UUID::random();
          sendResourceProviderStateUpdate();
        }
      }

      LOG(INFO)
        << "Finished reconciliation of storage pools for resource provider "
        << info.id();

      reconciling = false;

      return Nothing();
    }));
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

  // Reconcile resources after obtaining the resource provider ID.
  // TODO(chhsiao): Do the reconciliation early.
  reconcileResourceProviderState()
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
  Option<OperationID> operationId = operation.info().has_id()
    ? operation.info().id() : Option<OperationID>::none();

  if (state == SUBSCRIBED) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operationId,
        "Cannot apply operation in SUBSCRIBED state");
  }

  if (reconciling) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operationId,
        "Cannot apply operation when reconciling storage pools");
  }

  Try<id::UUID> operationVersion =
    id::UUID::fromBytes(operation.resource_version_uuid().value());
  CHECK_SOME(operationVersion);

  if (operationVersion.get() != resourceVersion) {
    return dropOperation(
        uuid.get(),
        frameworkId,
        operationId,
        "Mismatched resource version " + stringify(operationVersion.get()) +
        " (expected: " + stringify(resourceVersion) + ")");
  }

  CHECK(!operations.contains(uuid.get()));
  operations[uuid.get()] = protobuf::createOperation(
      operation.info(),
      protobuf::createOperationStatus(OPERATION_PENDING, operationId),
      frameworkId,
      slaveId,
      protobuf::createUUID(uuid.get()));

  checkpointResourceProviderState();

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
    .then(defer(self(), [=](bool continuation) -> Future<Nothing> {
      if (!continuation) {
        operations.erase(operationUuid.get());

        // Garbage collect the operation metadata.
        const string path = slave::paths::getOperationPath(
            slave::paths::getResourceProviderPath(
                metaDir, slaveId, info.type(), info.name(), info.id()),
            operationUuid.get());

        // NOTE: We check if the path exists since we do not checkpoint
        // some status updates, such as OPERATION_DROPPED.
        if (os::exists(path)) {
          Try<Nothing> rmdir = os::rmdir(path);
          if (rmdir.isError()) {
            return Failure(
                "Failed to remove directory '" + path + "': " + rmdir.error());
          }
        }
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
      return client.Probe(csi::v0::ProbeRequest())
        .then(defer(self(), [=](const csi::v0::ProbeResponse& response) {
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
      std::function<Future<Nothing>()>(defer(self(), [=]() {
        CHECK(services.at(containerId)->future().isPending());

        return connect(endpointPath)
          .then(defer(self(), [=](const csi::v0::Client& client) {
            services.at(containerId)->set(client);
            return Nothing();
          }))
          .onFailed(defer(self(), [=](const string& failure) {
            services.at(containerId)->fail(failure);
          }))
          .onDiscarded(defer(self(), [=] {
            services.at(containerId)->discard();
          }));
      })),
      std::function<Future<Nothing>()>(defer(self(), [=]() -> Future<Nothing> {
        if (containerId == controllerContainerId.get()) {
          metrics.csi_controller_plugin_terminations++;
        }

        if (containerId == nodeContainerId.get()) {
          metrics.csi_node_plugin_terminations++;
        }

        services.at(containerId)->discard();
        services.at(containerId).reset(new Promise<csi::v0::Client>());

        if (os::exists(endpointPath)) {
          Try<Nothing> rm = os::rm(endpointPath);
          if (rm.isError()) {
            return Failure(
                "Failed to remove endpoint '" + endpointPath +
                "': " + rm.error());
          }
        }

        return Nothing();
      })));

  if (daemon.isError()) {
    return Failure(
        "Failed to create container daemon for plugin container '" +
        stringify(containerId) + "': " + daemon.error());
  }

  // Checkpoint the plugin container config.
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


// Kills the specified plugin container and returns a future that waits
// for it to terminate.
Future<Nothing> StorageLocalResourceProviderProcess::killService(
    const ContainerID& containerId)
{
  CHECK(!daemons.contains(containerId));
  CHECK(!services.contains(containerId));

  agent::Call call;
  call.set_type(agent::Call::KILL_CONTAINER);
  call.mutable_kill_container()->mutable_container_id()->CopyFrom(containerId);

  return http::post(
      extractParentEndpoint(url),
      getAuthHeader(authToken),
      serialize(contentType, evolve(call)),
      stringify(contentType))
    .then(defer(self(), [=](const http::Response& response) -> Future<Nothing> {
      if (response.status == http::NotFound().status) {
        return Nothing();
      }

      if (response.status != http::OK().status) {
        return Failure(
            "Failed to kill container '" + stringify(containerId) +
            "': Unexpected response '" + response.status + "' (" + response.body
            + ")");
      }

      agent::Call call;
      call.set_type(agent::Call::WAIT_CONTAINER);
      call.mutable_wait_container()
        ->mutable_container_id()->CopyFrom(containerId);

      return http::post(
          extractParentEndpoint(url),
          getAuthHeader(authToken),
          serialize(contentType, evolve(call)),
          stringify(contentType))
        .then(defer(self(), [=](
            const http::Response& response) -> Future<Nothing> {
          if (response.status != http::OK().status &&
              response.status != http::NotFound().status) {
            return Failure(
                "Failed to wait for container '" + stringify(containerId) +
                "': Unexpected response '" + response.status + "' (" +
                response.body + ")");
          }

          return Nothing();
        }));
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::prepareIdentityService()
{
  CHECK_SOME(nodeContainerId);

  return getService(nodeContainerId.get())
    .then(defer(self(), [=](csi::v0::Client client) {
      // Get the plugin info.
      return client.GetPluginInfo(csi::v0::GetPluginInfoRequest())
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
      return client.GetPluginCapabilities(
          csi::v0::GetPluginCapabilitiesRequest())
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
      return client.GetPluginInfo(csi::v0::GetPluginInfoRequest())
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
      return client.ControllerGetCapabilities(
          csi::v0::ControllerGetCapabilitiesRequest())
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
      return client.NodeGetCapabilities(csi::v0::NodeGetCapabilitiesRequest())
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
          return client.NodeGetId(csi::v0::NodeGetIdRequest())
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

      return client.ControllerPublishVolume(request)
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

      return client.ControllerUnpublishVolume(request)
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

      return client.NodeStageVolume(request)
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

      return client.NodeUnstageVolume(request)
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

      return client.NodePublishVolume(request)
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

      return client.NodeUnpublishVolume(request)
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

      return client.CreateVolume(request)
        .then(defer(self(), [=](const csi::v0::CreateVolumeResponse& response) {
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
          .then(defer(self(), [volumeId](csi::v0::Client client) {
            csi::v0::DeleteVolumeRequest request;
            request.set_volume_id(volumeId);

            return client.DeleteVolume(request)
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
// applying `CREATE_VOLUME` or `CREATE_BLOCK` on a pre-existing volume, so we
// make it returns a volume ID, similar to `createVolume`.
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

      return client.ValidateVolumeCapabilities(request)
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
      return client.ListVolumes(csi::v0::ListVolumesRequest())
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

      foreach (const string& profile, knownProfiles) {
        CHECK(profileInfos.contains(profile));

        // TODO(chhsiao): Skip inactive profiles.

        const DiskProfileAdaptor::ProfileInfo& profileInfo =
          profileInfos.at(profile);

        csi::v0::GetCapacityRequest request;
        request.add_volume_capabilities()->CopyFrom(profileInfo.capability);
        *request.mutable_parameters() = profileInfo.parameters;

        futures.push_back(client.GetCapacity(request)
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
  Option<Resource> source;

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
    case Offer::Operation::CREATE_VOLUME: {
      CHECK(operation.info().has_create_volume());

      source = operation.info().create_volume().source();
      conversions = applyCreateVolumeOrBlock(
          source.get(),
          operationUuid,
          operation.info().create_volume().target_type());

      break;
    }
    case Offer::Operation::DESTROY_VOLUME: {
      CHECK(operation.info().has_destroy_volume());

      source = operation.info().destroy_volume().volume();
      conversions = applyDestroyVolumeOrBlock(source.get());

      break;
    }
    case Offer::Operation::CREATE_BLOCK: {
      CHECK(operation.info().has_create_block());

      source = operation.info().create_block().source();
      conversions = applyCreateVolumeOrBlock(
          source.get(),
          operationUuid,
          Resource::DiskInfo::Source::BLOCK);

      break;
    }
    case Offer::Operation::DESTROY_BLOCK: {
      CHECK(operation.info().has_destroy_block());

      source = operation.info().destroy_block().block();
      conversions = applyDestroyVolumeOrBlock(source.get());

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

  // NOTE: The code below is executed only when applying a storage operation.
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
      } else {
        LOG(ERROR)
          << "Failed to apply operation (uuid: " << operationUuid << "): "
          << conversions.error();
      }

      promise->associate(
          updateOperationStatus(operationUuid, conversions));
    }));

  Future<Nothing> future = promise->future();

  CHECK_SOME(source);
  if (source->disk().source().has_profile()) {
    // We place the future in `operationSequence` so it can be waited
    // for during reconciliation upon profile updates.
    operationSequence.add(
        std::function<Future<Nothing>()>([future] { return future; }));
  }

  return future;
}


// Sends `OPERATION_DROPPED` without checkpointing the status of
// the operation.
void StorageLocalResourceProviderProcess::dropOperation(
    const id::UUID& operationUuid,
    const Option<FrameworkID>& frameworkId,
    const Option<OperationID>& operationId,
    const string& message)
{
  LOG(WARNING)
    << "Dropping operation (uuid: " << operationUuid << "): " << message;

  UpdateOperationStatusMessage update =
    protobuf::createUpdateOperationStatusMessage(
       protobuf::createUUID(operationUuid),
       protobuf::createOperationStatus(
           OPERATION_DROPPED,
           operationId,
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
}


Future<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyCreateVolumeOrBlock(
    const Resource& resource,
    const id::UUID& operationUuid,
    const Resource::DiskInfo::Source::Type& type)
{
  if (resource.disk().source().type() != Resource::DiskInfo::Source::RAW) {
    return Failure(
        "Cannot create volume or block from source of " +
        stringify(resource.disk().source().type()) + " type");
  }

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
    case Resource::DiskInfo::Source::PATH:
    case Resource::DiskInfo::Source::MOUNT: {
      if (resource.disk().source().has_profile()) {
        CHECK(profileInfos.contains(resource.disk().source().profile()));

        // TODO(chhsiao): Reject if the source has an inactive profile.

        if (!profileInfos.at(resource.disk().source().profile())
               .capability.has_mount()) {
          return Failure(
              "Profile '" + resource.disk().source().profile() +
              "' cannot be used for CREATE_VOLUME operation");
        }

        // TODO(chhsiao): Call `CreateVolume` sequentially with other
        // create or delete operations, and send an `UPDATE_STATE` for
        // RAW profiled resources afterward.
        created = createVolume(
            operationUuid.toString(),
            Bytes(resource.scalar().value() * Bytes::MEGABYTES),
            profileInfos.at(resource.disk().source().profile()));
      } else {
        const string& volumeId = resource.disk().source().id();

        if (volumes.contains(volumeId)) {
          if (!volumes.at(volumeId).state.volume_capability().has_mount()) {
            return Failure(
                "Volume '" + volumeId + "' cannot be converted to a " +
                stringify(type) + " disk resource");
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
        CHECK(profileInfos.contains(resource.disk().source().profile()));

        // TODO(chhsiao): Reject if the source has an inactive profile.

        if (!profileInfos.at(resource.disk().source().profile())
               .capability.has_block()) {
          return Failure(
              "Profile '" + resource.disk().source().profile() +
              "' cannot be used for CREATE_BLOCK operation");
        }

        // TODO(chhsiao): Call `CreateVolume` sequentially with other
        // create or delete operations, and send an `UPDATE_STATE` for
        // RAW profiled resources afterward.
        created = createVolume(
            operationUuid.toString(),
            Bytes(resource.scalar().value() * Bytes::MEGABYTES),
            profileInfos.at(resource.disk().source().profile()));
      } else {
        const string& volumeId = resource.disk().source().id();

        if (volumes.contains(volumeId)) {
          if (!volumes.at(volumeId).state.volume_capability().has_block()) {
            return Failure(
                "Volume '" + volumeId + "' cannot be converted to a " +
                stringify(type) + " disk resource");
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
        case Resource::DiskInfo::Source::PATH: {
          // Set the root path relative to agent work dir.
          converted.mutable_disk()->mutable_source()->mutable_path()
            ->set_root(mountRootDir);

          break;
        }
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
StorageLocalResourceProviderProcess::applyDestroyVolumeOrBlock(
    const Resource& resource)
{
  switch (resource.disk().source().type()) {
    case Resource::DiskInfo::Source::PATH:
    case Resource::DiskInfo::Source::MOUNT:
    case Resource::DiskInfo::Source::BLOCK: {
      break;
    }
    case Resource::DiskInfo::Source::UNKNOWN:
    case Resource::DiskInfo::Source::RAW: {
      return Failure(
          "Cannot destroy volume or block of " +
          stringify(resource.disk().source().type()) + " type");
      break;
    }
  }

  CHECK(resource.disk().source().has_id());
  CHECK(volumes.contains(resource.disk().source().id()));

  // Sequentialize the deletion with other operation on the same volume.
  // NOTE: A resource has no profile iff it is a pre-existing volume.
  return volumes.at(resource.disk().source().id()).sequence->add(
      std::function<Future<Nothing>()>(defer(
          self(),
          &Self::deleteVolume,
          resource.disk().source().id(),
          !resource.disk().source().has_profile())))
    .then(defer(self(), [=]() {
      // TODO(chhsiao): Convert to an empty resource and update all
      // storage pools if the profile has been deactivated.
      Resource converted = resource;
      converted.mutable_disk()->mutable_source()->set_type(
          Resource::DiskInfo::Source::RAW);
      converted.mutable_disk()->mutable_source()->clear_path();
      converted.mutable_disk()->mutable_source()->clear_mount();

      // NOTE: We keep the source ID and metadata if it is a
      // pre-existing volume, which has no profile.
      if (resource.disk().source().has_profile()) {
        converted.mutable_disk()->mutable_source()->clear_id();
        converted.mutable_disk()->mutable_source()->clear_metadata();
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

  if (error.isSome()) {
    // We only send `UPDATE_STATE` for failed speculative operations.
    if (operation.info().type() == Offer::Operation::RESERVE ||
        operation.info().type() == Offer::Operation::UNRESERVE ||
        operation.info().type() == Offer::Operation::CREATE ||
        operation.info().type() == Offer::Operation::DESTROY) {
      resourceVersion = id::UUID::random();
      sendResourceProviderStateUpdate();
    }

    return error.get();
  }

  return Nothing();
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

  Try<Nothing> checkpoint = slave::state::checkpoint(statePath, state);
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

  Try<Nothing> checkpoint =
    slave::state::checkpoint(statePath, volumes.at(volumeId).state);

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

  auto err = [](const ResourceProviderID& id, const string& message) {
    LOG(ERROR)
      << "Failed to update state for resource provider " << id << ": "
      << message;
  };

  driver->send(evolve(call))
    .onFailed(std::bind(err, info.id(), lambda::_1))
    .onDiscarded(std::bind(err, info.id(), "future discarded"));
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
  : csi_controller_plugin_terminations(
        prefix + "csi_controller_plugin_terminations"),
    csi_node_plugin_terminations(
        prefix + "csi_node_plugin_terminations")
{
  process::metrics::add(csi_controller_plugin_terminations);
  process::metrics::add(csi_node_plugin_terminations);
}


StorageLocalResourceProviderProcess::Metrics::~Metrics()
{
  process::metrics::remove(csi_controller_plugin_terminations);
  process::metrics::remove(csi_node_plugin_terminations);
}


Try<Owned<LocalResourceProvider>> StorageLocalResourceProvider::create(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken,
    bool strict)
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

  return Owned<LocalResourceProvider>(new StorageLocalResourceProvider(
      url, workDir, info, slaveId, authToken, strict));
}


Try<Principal> StorageLocalResourceProvider::principal(
    const ResourceProviderInfo& info)
{
  return Principal(
      Option<string>::none(),
      {{"cid_prefix", getContainerIdPrefix(info)}});
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
