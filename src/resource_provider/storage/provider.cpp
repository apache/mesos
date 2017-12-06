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
#include <stout/path.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/rmdir.hpp>

#include "common/http.hpp"

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
  void initialize() override;
  void fatal(const string& messsage, const string& failure);

  Future<Nothing> recover();
  Future<Nothing> recoverServices();
  void doReliableRegistration();

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void operation(const Event::Operation& operation);
  void publish(const Event::Publish& publish);

  Future<csi::Client> connect(const string& endpoint);
  Future<csi::Client> getService(const ContainerID& containerId);
  Future<Nothing> killService(const ContainerID& containerId);

  Future<Nothing> prepareControllerService();
  Future<Nothing> prepareNodeService();

  void checkpointResourceProviderState();

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
  string resourceVersion;
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

  const string message =
    "Failed to recover resource provider with type '" + info.type() +
    "' and name '" + info.name() + "'";

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
      // Recover the resource provider ID from the latest symlink. If
      // the symlink does not exist or it points to a non-exist
      // directory, treat this as a new resource provider.
      // TODO(chhsiao): State recovery.
      Result<string> realpath = os::realpath(
          slave::paths::getLatestResourceProviderPath(
              metaDir, slaveId, info.type(), info.name()));

      if (realpath.isError()) {
        return Failure(
            "Failed to read the latest symlink for resource provider with "
            "type '" + info.type() + "' and name '" + info.name() + "'"
            ": " + realpath.error());
      }

      if (realpath.isNone()) {
        resourceVersion = UUID::random().toBytes();
      } else {
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

        if (resourceProviderState.isNone()) {
          resourceVersion = UUID::random().toBytes();
        } else {
          foreach (const Event::Operation& operation,
                   resourceProviderState->operations()) {
            pendingOperations.push_back(operation);
          }

          totalResources = resourceProviderState->resources();
          resourceVersion = resourceProviderState->resource_version_uuid();
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

  const string message =
    "Failed to subscribe resource provider with type '" + info.type() +
    "' and name '" + info.name() + "'";

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_resource_provider_info()->CopyFrom(info);

  driver->send(evolve(call));

  // TODO(chhsiao): Consider doing an exponential backoff.
  delay(Seconds(1), self(), &Self::doReliableRegistration);
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


void StorageLocalResourceProviderProcess::checkpointResourceProviderState()
{
  ResourceProviderState state;

  foreach (const Event::Operation& operation, pendingOperations) {
    state.add_operations()->CopyFrom(operation);
  }

  state.mutable_resources()->CopyFrom(totalResources);
  state.set_resource_version_uuid(resourceVersion);

  const string statePath = slave::paths::getResourceProviderStatePath(
      metaDir, slaveId, info.type(), info.name(), info.id());

  CHECK_SOME(slave::state::checkpoint(statePath, state))
    << "Failed to checkpoint resource provider state to '" << statePath << "'";
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
