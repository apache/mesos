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
#include <numeric>

#include <glog/logging.h>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "csi/client.hpp"
#include "csi/paths.hpp"
#include "csi/utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/detector.hpp"
#include "resource_provider/state.hpp"

#include "slave/container_daemon.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

namespace http = process::http;

using std::accumulate;
using std::find;
using std::list;
using std::queue;
using std::string;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;
using process::Timeout;

using process::after;
using process::collect;
using process::defer;
using process::loop;
using process::spawn;

using process::http::authentication::Principal;

using mesos::internal::protobuf::convertLabelsToStringMap;
using mesos::internal::protobuf::convertStringMapToLabels;

using mesos::internal::slave::ContainerDaemon;

using mesos::resource_provider::Call;
using mesos::resource_provider::Event;

using mesos::resource_provider::state::ResourceProviderState;

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
static const Duration CSI_ENDPOINT_CREATION_TIMEOUT = Seconds(5);


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
    const ResourceProviderID& resourceProviderId,
    double capacity,
    const Option<string>& profile,
    const Option<string>& id = None(),
    const Option<Labels>& metadata = None())
{
  Resource resource;
  resource.set_name("disk");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()->set_value(capacity);
  resource.mutable_provider_id()->CopyFrom(resourceProviderId),
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
      const Option<string>& _authToken)
    : ProcessBase(process::ID::generate("storage-local-resource-provider")),
      state(RECOVERING),
      url(_url),
      workDir(_workDir),
      metaDir(slave::paths::getMetaRootDir(_workDir)),
      contentType(ContentType::PROTOBUF),
      info(_info),
      slaveId(_slaveId),
      authToken(_authToken) {}

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const Event& event);

private:
  struct ProfileData
  {
    csi::VolumeCapability capability;
    google::protobuf::Map<string, string> parameters;
  };

  void initialize() override;
  void fatal(const string& messsage, const string& failure);

  Future<Nothing> recover();
  Future<Nothing> recoverServices();
  void doReliableRegistration();
  Future<Nothing> reconcile();

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void operation(const Event::Operation& operation);
  void publish(const Event::Publish& publish);

  Future<csi::Client> connect(const string& endpoint);
  Future<csi::Client> getService(const ContainerID& containerId);
  Future<Nothing> killService(const ContainerID& containerId);

  Future<Nothing> prepareControllerService();
  Future<Nothing> prepareNodeService();
  Future<Resources> importResources();

  void checkpointResourceProviderState();
  void sendResourceProviderStateUpdate();

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

  csi::Version csiVersion;
  hashmap<string, ProfileData> profiles;
  process::grpc::client::Runtime runtime;
  Owned<v1::resource_provider::Driver> driver;

  ContainerID controllerContainerId;
  ContainerID nodeContainerId;
  hashmap<ContainerID, Owned<ContainerDaemon>> daemons;
  hashmap<ContainerID, Owned<Promise<csi::Client>>> services;

  Option<csi::GetPluginInfoResponse> controllerInfo;
  Option<csi::GetPluginInfoResponse> nodeInfo;
  Option<csi::ControllerCapabilities> controllerCapabilities;
  Option<string> nodeId;

  list<Event::Operation> pendingOperations;
  Resources totalResources;
  Option<UUID> resourceVersion;
};


void StorageLocalResourceProviderProcess::connected()
{
  CHECK_EQ(DISCONNECTED, state);

  state = CONNECTED;

  doReliableRegistration();
}


void StorageLocalResourceProviderProcess::disconnected()
{
  CHECK(state == CONNECTED || state == SUBSCRIBED || state == READY);

  LOG(INFO) << "Disconnected from resource provider manager";

  state = DISCONNECTED;
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
    case Event::OPERATION: {
      CHECK(event.has_operation());
      operation(event.operation());
      break;
    }
    case Event::PUBLISH: {
      CHECK(event.has_publish());
      publish(event.publish());
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
  // Set CSI version to 0.1.0.
  csiVersion.set_major(0);
  csiVersion.set_minor(1);
  csiVersion.set_patch(0);

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    auto it = find(
        container.services().begin(),
        container.services().end(),
        CSIPluginContainerInfo::CONTROLLER_SERVICE);
    if (it != container.services().end()) {
      controllerContainerId = getContainerId(info, container);
      break;
    }
  }

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    auto it = find(
        container.services().begin(),
        container.services().end(),
        CSIPluginContainerInfo::NODE_SERVICE);
    if (it != container.services().end()) {
      nodeContainerId = getContainerId(info, container);
      break;
    }
  }

  // NOTE: The name of the default profile is an empty string, which is
  // the default value for `Resource.disk.source.profile` when unset.
  // TODO(chhsiao): Use the volume profile module.
  ProfileData& defaultProfile = profiles[""];
  defaultProfile.capability.mutable_mount();
  defaultProfile.capability.mutable_access_mode()
    ->set_mode(csi::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);

  const string message =
    "Failed to recover resource provider with type '" + info.type() +
    "' and name '" + info.name() + "'";

  // NOTE: Most resource provider events rely on the plugins being
  // prepared. To avoid race conditions, we connect to the agent after
  // preparing the plugins.
  recover()
    .onFailed(defer(self(), &Self::fatal, message, lambda::_1))
    .onDiscarded(defer(self(), &Self::fatal, message, "future discarded"));
}


void StorageLocalResourceProviderProcess::fatal(
    const string& message,
    const string& failure)
{
  LOG(ERROR) << message << ": " << failure;

  // Force the disconnection early.
  driver.reset();

  process::terminate(self());
}


Future<Nothing> StorageLocalResourceProviderProcess::recover()
{
  CHECK_EQ(RECOVERING, state);

  return recoverServices()
    .then(defer(self(), [=]() -> Future<Nothing> {
      // Recover the resource provider ID and state from the latest
      // symlink. If the symlink cannot be resolved, this is a new
      // resource provider, and `totalResources` and `resourceVersion`
      // will be empty, which is fine since they will be set up during
      // reconciliation.
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

        Result<ResourceProviderState> resourceProviderState =
          ::protobuf::read<ResourceProviderState>(statePath);

        if (resourceProviderState.isError()) {
          return Failure(
              "Failed to read resource provider state from '" + statePath +
              "': " + resourceProviderState.error());
        }

        if (resourceProviderState.isSome()) {
          foreach (const Event::Operation& operation,
                   resourceProviderState->operations()) {
            pendingOperations.push_back(operation);
          }

          totalResources = resourceProviderState->resources();

          Try<UUID> uuid =
            UUID::fromBytes(resourceProviderState->resource_version_uuid());
          CHECK_SOME(uuid);

          resourceVersion = uuid.get();
        }
      }

      state = DISCONNECTED;

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
          None())); // TODO(nfnt): Add authentication as part of MESOS-7854.

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

  list<Future<Nothing>> futures;

  foreach (const string& path, containerPaths.get()) {
    ContainerID containerId;
    containerId.set_value(Path(path).basename());

    // Do not kill the up-to-date controller or node container.
    // Otherwise, kill them and perform cleanups.
    if (containerId == controllerContainerId ||
        containerId == nodeContainerId) {
      const string configPath = csi::paths::getContainerInfoPath(
          slave::paths::getCsiRootDir(workDir),
          info.storage().plugin().type(),
          info.storage().plugin().name(),
          containerId);

      Result<CSIPluginContainerInfo> config =
        ::protobuf::read<CSIPluginContainerInfo>(configPath);

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

  return collect(futures)
    .then(defer(self(), &Self::prepareNodeService))
    .then(defer(self(), &Self::prepareControllerService));
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


Future<Nothing> StorageLocalResourceProviderProcess::reconcile()
{
  return importResources()
    .then(defer(self(), [=](Resources imported) {
      // NOTE: We do not support decreasing the total resources for now.
      // Any resource in the checkpointed state will be reported to the
      // resource provider manager, even if it is missing in the
      // imported resources from the plugin. Additional resources from
      // the plugin will be reported with the default reservation.

      Resources stripped;
      foreach (const Resource& resource, totalResources) {
        CHECK(resource.disk().source().has_id());

        stripped += createRawDiskResource(
            resource.provider_id(),
            resource.scalar().value(),
            resource.disk().source().has_profile()
              ? resource.disk().source().profile() : Option<string>::none(),
            resource.disk().source().has_id()
              ? resource.disk().source().id() : Option<string>::none(),
            resource.disk().source().has_metadata()
              ? resource.disk().source().metadata() : Option<Labels>::none());
      }

      Resources result = totalResources;
      foreach (Resource resource, imported - stripped) {
        resource.mutable_reservations()->CopyFrom(info.default_reservations());
        result += resource;

        LOG(INFO) << "Adding new resource '" << resource << "'";
      }

      if (resourceVersion.isNone() || result != totalResources) {
        totalResources = result;
        resourceVersion = UUID::random();
        checkpointResourceProviderState();
      }

      sendResourceProviderStateUpdate();

      state = READY;

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

  const string message =
    "Failed to update state for resource provider " + stringify(info.id());

  // Reconcile resources after obtaining the resource provider ID.
  // TODO(chhsiao): Do the reconciliation early.
  reconcile()
    .onFailed(defer(self(), &Self::fatal, message, lambda::_1))
    .onDiscarded(defer(self(), &Self::fatal, message, "future discarded"));
}


void StorageLocalResourceProviderProcess::operation(
    const Event::Operation& operation)
{
  if (state == SUBSCRIBED) {
    // TODO(chhsiao): Reject this operation.
    return;
  }

  CHECK_EQ(READY, state);

  pendingOperations.push_back(operation);
  checkpointResourceProviderState();
}


void StorageLocalResourceProviderProcess::publish(const Event::Publish& publish)
{
  if (state == SUBSCRIBED) {
    // TODO(chhsiao): Reject this publish request.
    return;
  }

  CHECK_EQ(READY, state);
}


// Returns a future of a CSI client that waits for the endpoint socket
// to appear if necessary, then connects to the socket and check its
// supported version.
Future<csi::Client> StorageLocalResourceProviderProcess::connect(
    const string& endpoint)
{
  Future<csi::Client> client;

  if (os::exists(endpoint)) {
    client = csi::Client("unix://" + endpoint, runtime);
  } else {
    // Wait for the endpoint socket to appear until the timeout expires.
    Timeout timeout = Timeout::in(CSI_ENDPOINT_CREATION_TIMEOUT);

    client = loop(
        self(),
        [=]() -> Future<Nothing> {
          if (timeout.expired()) {
            return Failure("Timed out waiting for endpoint '" + endpoint + "'");
          }

          return after(Milliseconds(10));
        },
        [=](const Nothing&) -> ControlFlow<csi::Client> {
          if (os::exists(endpoint)) {
            return Break(csi::Client("unix://" + endpoint, runtime));
          }

          return Continue();
        });
  }

  return client
    .then(defer(self(), [=](csi::Client client) {
      return client.GetSupportedVersions(csi::GetSupportedVersionsRequest())
        .then(defer(self(), [=](
            const csi::GetSupportedVersionsResponse& response)
            -> Future<csi::Client> {
          auto it = find(
              response.supported_versions().begin(),
              response.supported_versions().end(),
              csiVersion);
          if (it == response.supported_versions().end()) {
            return Failure(
                "CSI version " + stringify(csiVersion) + " is not supported");
          }

          return client;
        }));
    }));
}


// Returns a future of the latest CSI client for the specified plugin
// container. If the container is not already running, this method will
// start a new a new container daemon.
Future<csi::Client> StorageLocalResourceProviderProcess::getService(
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
  const string mountDir = csi::paths::getMountRootDir(
      slave::paths::getCsiRootDir(workDir),
      info.storage().plugin().type(),
      info.storage().plugin().name());

  Try<Nothing> mkdir = os::mkdir(mountDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" + mountDir +
        "': " + mkdir.error());
  }

  // Prepare a volume where the mount points will be placed.
  Volume* mountVolume = containerInfo.add_volumes();
  mountVolume->set_mode(Volume::RW);
  mountVolume->set_container_path(mountDir);
  mountVolume->mutable_source()->set_type(Volume::Source::HOST_PATH);
  mountVolume->mutable_source()->mutable_host_path()->set_path(mountDir);
  mountVolume->mutable_source()->mutable_host_path()
    ->mutable_mount_propagation()->set_mode(MountPropagation::BIDIRECTIONAL);

  CHECK(!services.contains(containerId));
  services[containerId].reset(new Promise<csi::Client>());

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
          .then(defer(self(), [=](const csi::Client& client) {
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
        services.at(containerId)->discard();
        services.at(containerId).reset(new Promise<csi::Client>());

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

  const string message =
    "Container daemon for '" + stringify(containerId) + "' failed";

  daemons[containerId] = daemon.get();
  daemon.get()->wait()
    .onFailed(defer(self(), &Self::fatal, message, lambda::_1))
    .onDiscarded(defer(self(), &Self::fatal, message, "future discarded"));

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


Future<Nothing> StorageLocalResourceProviderProcess::prepareControllerService()
{
  return getService(controllerContainerId)
    .then(defer(self(), [=](csi::Client client) {
      // Get the plugin info and check for consistency.
      csi::GetPluginInfoRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.GetPluginInfo(request)
        .then(defer(self(), [=](const csi::GetPluginInfoResponse& response) {
          controllerInfo = response;

          LOG(INFO)
            << "Controller plugin loaded: " << stringify(controllerInfo.get());

          if (nodeInfo.isSome() &&
              (controllerInfo->name() != nodeInfo->name() ||
               controllerInfo->vendor_version() !=
                 nodeInfo->vendor_version())) {
            LOG(WARNING)
              << "Inconsistent controller and node plugin components. Please "
                 "check with the plugin vendor to ensure compatibility.";
          }

          // NOTE: We always get the latest service future before
          // proceeding to the next step.
          return getService(controllerContainerId);
        }));
    }))
    .then(defer(self(), [=](csi::Client client) {
      // Probe the plugin to validate the runtime environment.
      csi::ControllerProbeRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.ControllerProbe(request)
        .then(defer(self(), [=](const csi::ControllerProbeResponse& response) {
          return getService(controllerContainerId);
        }));
    }))
    .then(defer(self(), [=](csi::Client client) {
      // Get the controller capabilities.
      csi::ControllerGetCapabilitiesRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.ControllerGetCapabilities(request)
        .then(defer(self(), [=](
            const csi::ControllerGetCapabilitiesResponse& response) {
          controllerCapabilities = response.capabilities();

          return Nothing();
        }));
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::prepareNodeService()
{
  return getService(nodeContainerId)
    .then(defer(self(), [=](csi::Client client) {
      // Get the plugin info and check for consistency.
      csi::GetPluginInfoRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.GetPluginInfo(request)
        .then(defer(self(), [=](const csi::GetPluginInfoResponse& response) {
          nodeInfo = response;

          LOG(INFO) << "Node plugin loaded: " << stringify(nodeInfo.get());

          if (controllerInfo.isSome() &&
              (controllerInfo->name() != nodeInfo->name() ||
               controllerInfo->vendor_version() !=
                 nodeInfo->vendor_version())) {
            LOG(WARNING)
              << "Inconsistent controller and node plugin components. Please "
                 "check with the plugin vendor to ensure compatibility.";
          }

          // NOTE: We always get the latest service future before
          // proceeding to the next step.
          return getService(nodeContainerId);
        }));
    }))
    .then(defer(self(), [=](csi::Client client) {
      // Probe the plugin to validate the runtime environment.
      csi::NodeProbeRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.NodeProbe(request)
        .then(defer(self(), [=](const csi::NodeProbeResponse& response) {
          return getService(nodeContainerId);
        }));
    }))
    .then(defer(self(), [=](csi::Client client) {
      // Get the node ID.
      csi::GetNodeIDRequest request;
      request.mutable_version()->CopyFrom(csiVersion);

      return client.GetNodeID(request)
        .then(defer(self(), [=](const csi::GetNodeIDResponse& response) {
          nodeId = response.node_id();

          return Nothing();
        }));
    }));
}


// Returns resources reported by the CSI plugin, which are unreserved
// raw disk resources without any persistent volume.
Future<Resources> StorageLocalResourceProviderProcess::importResources()
{
  // NOTE: This can only be called after `prepareControllerService` and
  // the resource provider ID has been obtained.
  CHECK_SOME(controllerCapabilities);
  CHECK(info.has_id());

  Future<Resources> preprovisioned;

  if (controllerCapabilities->listVolumes) {
    preprovisioned = getService(controllerContainerId)
      .then(defer(self(), [=](csi::Client client) {
        // TODO(chhsiao): Set the max entries and use a loop to do
        // mutliple `ListVolumes` calls.
        csi::ListVolumesRequest request;
        request.mutable_version()->CopyFrom(csiVersion);

        return client.ListVolumes(request)
          .then(defer(self(), [=](const csi::ListVolumesResponse& response) {
            Resources resources;

            // Recover volume profiles from the checkpointed state.
            hashmap<string, string> volumesToProfiles;
            foreach (const Resource& resource, totalResources) {
              if (resource.disk().source().has_id() &&
                  resource.disk().source().has_profile()) {
                volumesToProfiles[resource.disk().source().id()] =
                  resource.disk().source().profile();
              }
            }

            foreach (const auto& entry, response.entries()) {
              resources += createRawDiskResource(
                  info.id(),
                  entry.volume_info().capacity_bytes(),
                  volumesToProfiles.contains(entry.volume_info().id())
                    ? volumesToProfiles.at(entry.volume_info().id())
                    : Option<string>::none(),
                  entry.volume_info().id(),
                  entry.volume_info().attributes().empty()
                    ? Option<Labels>::none()
                    : convertStringMapToLabels(
                          entry.volume_info().attributes()));
            }

            return resources;
          }));
      }));
  } else {
    preprovisioned = Resources();
  }

  return preprovisioned
    .then(defer(self(), [=](const Resources& preprovisioned) {
      list<Future<Resources>> futures;

      foreach (const Resource& resource, preprovisioned) {
        futures.push_back(getService(controllerContainerId)
          .then(defer(self(), [=](csi::Client client) {
            csi::ValidateVolumeCapabilitiesRequest request;
            request.mutable_version()->CopyFrom(csiVersion);
            request.set_volume_id(resource.disk().source().id());

            // The default profile is used if `profile` is unset.
            request.add_volume_capabilities()->CopyFrom(
                profiles.at(resource.disk().source().profile()).capability);

            if (resource.disk().source().has_metadata()) {
              request.mutable_volume_attributes()->swap(
                  convertLabelsToStringMap(
                      resource.disk().source().metadata()).get());
            }

            return client.ValidateVolumeCapabilities(request)
              .then(defer(self(), [=](
                  const csi::ValidateVolumeCapabilitiesResponse& response)
                  -> Future<Resources> {
                if (!response.supported()) {
                  return Failure(
                      "Unsupported volume capability for resource " +
                      stringify(resource) + ": " + response.message());
                }

                return resource;
              }));
          })));
      }

      if (controllerCapabilities->getCapacity) {
        foreachkey (const string& profile, profiles) {
          futures.push_back(getService(controllerContainerId)
            .then(defer(self(), [=](csi::Client client) {
              csi::GetCapacityRequest request;
              request.mutable_version()->CopyFrom(csiVersion);
              request.add_volume_capabilities()
                ->CopyFrom(profiles.at(profile).capability);
              *request.mutable_parameters() = profiles.at(profile).parameters;

              return client.GetCapacity(request)
                .then(defer(self(), [=](
                    const csi::GetCapacityResponse& response)
                    -> Future<Resources> {
                  if (response.available_capacity() == 0) {
                    return Resources();
                  }

                  return createRawDiskResource(
                      info.id(),
                      response.available_capacity(),
                      profile.empty() ? Option<string>::none() : profile);
              }));
            })));
        }
      }

      return collect(futures)
        .then(defer(self(), [=](const list<Resources>& resources) {
          return accumulate(resources.begin(), resources.end(), Resources());
        }));
    }));
}


void StorageLocalResourceProviderProcess::checkpointResourceProviderState()
{
  ResourceProviderState state;

  foreach (const Event::Operation& operation, pendingOperations) {
    state.add_operations()->CopyFrom(operation);
  }

  state.mutable_resources()->CopyFrom(totalResources);

  CHECK_SOME(resourceVersion);
  state.set_resource_version_uuid(resourceVersion->toBytes());

  const string statePath = slave::paths::getResourceProviderStatePath(
      metaDir, slaveId, info.type(), info.name(), info.id());

  CHECK_SOME(slave::state::checkpoint(statePath, state))
    << "Failed to checkpoint resource provider state to '" << statePath << "'";
}


void StorageLocalResourceProviderProcess::sendResourceProviderStateUpdate()
{
  Call call;
  call.set_type(Call::UPDATE_STATE);
  call.mutable_resource_provider_id()->CopyFrom(info.id());

  Call::UpdateState* update = call.mutable_update_state();

  foreach (const Event::Operation& operation, pendingOperations) {
    Try<UUID> operationUuid = UUID::fromBytes(operation.operation_uuid());
    CHECK_SOME(operationUuid);

    // TODO(chhsiao): Maintain a list of terminated but unacknowledged
    // offer operations in memory and reconstruc that during recovery
    // by querying status update manager.
    update->add_operations()->CopyFrom(
        protobuf::createOfferOperation(
            operation.info(),
            protobuf::createOfferOperationStatus(
                OFFER_OPERATION_PENDING,
                operation.info().has_id()
                  ? Option<OfferOperationID>(operation.info().id())
                  : None()),
            operation.framework_id(),
            slaveId,
            operationUuid.get()));
  }

  update->mutable_resources()->CopyFrom(totalResources);

  CHECK_SOME(resourceVersion);
  update->set_resource_version_uuid(resourceVersion->toBytes());

  auto err = [](const ResourceProviderID& id, const string& message) {
    LOG(ERROR)
      << "Failed to update state for resource provider " << id << ": "
      << message;
  };

  driver->send(evolve(call))
    .onFailed(std::bind(err, info.id(), lambda::_1))
    .onDiscarded(std::bind(err, info.id(), "future discarded"));
}


Try<Owned<LocalResourceProvider>> StorageLocalResourceProvider::create(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken)
{
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

  bool hasControllerService = false;
  bool hasNodeService = false;

  foreach (const CSIPluginContainerInfo& container,
           info.storage().plugin().containers()) {
    for (int i = 0; i < container.services_size(); i++) {
      const CSIPluginContainerInfo::Service service = container.services(i);
      if (service == CSIPluginContainerInfo::CONTROLLER_SERVICE) {
        hasControllerService = true;
      } else if (service == CSIPluginContainerInfo::NODE_SERVICE) {
        hasNodeService = true;
      }
    }
  }

  if (!hasControllerService) {
    return Error(
        stringify(CSIPluginContainerInfo::CONTROLLER_SERVICE) + " not found");
  }

  if (!hasNodeService) {
    return Error(
        stringify(CSIPluginContainerInfo::NODE_SERVICE) + " not found");
  }

  return Owned<LocalResourceProvider>(
      new StorageLocalResourceProvider(url, workDir, info, slaveId, authToken));
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
    const Option<string>& authToken)
  : process(new StorageLocalResourceProviderProcess(
        url, workDir, info, slaveId, authToken))
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
