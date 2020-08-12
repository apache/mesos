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

#include "csi/service_manager.hpp"

#include <algorithm>
#include <functional>
#include <list>
#include <utility>
#include <vector>

#include <mesos/http.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/realpath.hpp>

#include "common/http.hpp"

#include "csi/paths.hpp"
#include "csi/v0_client.hpp"
#include "csi/v1_client.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "slave/container_daemon.hpp"
#include "slave/state.hpp"

namespace http = process::http;
namespace slave = mesos::internal::slave;

using std::list;
using std::string;
using std::vector;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::Timeout;

using process::grpc::StatusError;

using process::grpc::client::Runtime;

using slave::ContainerDaemon;

namespace mesos {
namespace csi {

// Timeout for a CSI plugin component to create its endpoint socket.
//
// TODO(chhsiao): Make the timeout configurable.
constexpr Duration CSI_ENDPOINT_CREATION_TIMEOUT = Minutes(1);


// Returns the container ID of the specified CSI plugin container. The container
// ID is of the following format:
//
//     <container_prefix><plugin_type>-<plugin_name>--<list_of_services>
//
// where <plugin_type> and <plugin_name> are the type and name of the CSI plugin
// with dots replaced by dashes, and <list_of_services> lists the CSI services
// provided by the component, concatenated with dashes.
static ContainerID getContainerId(
    const CSIPluginInfo& info,
    const string& containerPrefix,
    const CSIPluginContainerInfo& container)
{
  // NOTE: We cannot simply stringify `container.services()` since it returns
  // `RepeatedField<int>`, so we reconstruct the list of services here.
  vector<Service> services;
  services.reserve(container.services_size());
  for (int i = 0; i < container.services_size(); i++) {
    services.push_back(container.services(i));
  }

  ContainerID containerId;
  containerId.set_value(
      containerPrefix +
      strings::join("-", strings::replace(info.type(), ".", "-"), info.name()) +
      "--" + strings::join("-", services));

  return containerId;
}


class ServiceManagerProcess : public Process<ServiceManagerProcess>
{
public:
  ServiceManagerProcess(
      const SlaveID& _agentId,
      const http::URL& _agentUrl,
      const string& _rootDir,
      const CSIPluginInfo& _info,
      const hashset<Service>& services,
      const string& _containerPrefix,
      const Option<string>& _authToken,
      const Runtime& _runtime,
      Metrics* _metrics);

  ServiceManagerProcess(
      const CSIPluginInfo& _info,
      const hashset<Service>& services,
      const Runtime& _runtime,
      Metrics* _metrics);

  Future<Nothing> recover();

  Future<string> getServiceEndpoint(const Service& service);
  Future<string> getApiVersion();

private:
  // Returns the container info of the specified container for this CSI plugin.
  Option<CSIPluginContainerInfo> getContainerInfo(
      const ContainerID& containerId);

  // Returns a map of any existing container of this CSI plugin to its status,
  // or `None` if it does not have a status (e.g., being destroyed).
  Future<hashmap<ContainerID, Option<ContainerStatus>>> getContainers();

  // Waits for the specified plugin container to be terminated.
  Future<Nothing> waitContainer(const ContainerID& containerId);

  // Kills the specified plugin container.
  Future<Nothing> killContainer(const ContainerID& containerId);

  // Waits for the endpoint (URI to a Unix domain socket) to be created.
  Future<Nothing> waitEndpoint(const string& endpoint);

  // Probes the endpoint to detect the API version and check for readiness.
  Future<Nothing> probeEndpoint(const string& endpoint);

  // Returns the URI of the latest service endpoint for the specified plugin
  // container. If the container is not already running, this method will start
  // a new container.
  Future<string> getEndpoint(const ContainerID& containerId);

  const SlaveID agentId;
  const http::URL agentUrl;
  const string rootDir;
  const CSIPluginInfo info;
  const string containerPrefix;
  const Option<string> authToken;
  const ContentType contentType;

  Runtime runtime;
  Metrics* metrics;

  http::Headers headers;
  Option<string> apiVersion;

  // This is for the managed CSI plugin which will be launched as
  // standalone containers.
  hashmap<Service, ContainerID> serviceContainers;

  // This is for the unmanaged CSI plugin which is already deployed
  // out of Mesos.
  hashmap<Service, string> serviceEndpoints;

  hashmap<ContainerID, Owned<ContainerDaemon>> daemons;
  hashmap<ContainerID, Owned<Promise<string>>> endpoints;
};


ServiceManagerProcess::ServiceManagerProcess(
    const SlaveID& _agentId,
    const http::URL& _agentUrl,
    const string& _rootDir,
    const CSIPluginInfo& _info,
    const hashset<Service>& services,
    const string& _containerPrefix,
    const Option<string>& _authToken,
    const Runtime& _runtime,
    Metrics* _metrics)
  : ProcessBase(process::ID::generate("csi-service-manager")),
    agentId(_agentId),
    agentUrl(_agentUrl),
    rootDir(_rootDir),
    info(_info),
    containerPrefix(_containerPrefix),
    authToken(_authToken),
    contentType(ContentType::PROTOBUF),
    runtime(_runtime),
    metrics(_metrics)
{
  headers["Accept"] = stringify(contentType);
  if (authToken.isSome()) {
    headers["Authorization"] = "Bearer " + authToken.get();
  }

  foreach (const Service& service, services) {
    // Each service is served by the first container providing the service. See
    // `CSIPluginInfo` in `mesos.proto` for details.
    foreach (const CSIPluginContainerInfo& container, info.containers()) {
      if (container.services().end() != std::find(
              container.services().begin(),
              container.services().end(),
              service)) {
        serviceContainers[service] =
          getContainerId(info, containerPrefix, container);

        break;
      }
    }

    CHECK(serviceContainers.contains(service))
      << service << " not found for CSI plugin type '" << info.type()
      << "' and name '" << info.name() << "'";
  }
}


ServiceManagerProcess::ServiceManagerProcess(
    const CSIPluginInfo& _info,
    const hashset<Service>& services,
    const Runtime& _runtime,
    Metrics* _metrics)
  : ProcessBase(process::ID::generate("csi-service-manager")),
    agentId(),
    agentUrl(),
    rootDir(),
    info(_info),
    containerPrefix(),
    authToken(),
    contentType(ContentType::PROTOBUF),
    runtime(_runtime),
    metrics(_metrics)
{
  foreach (const Service& service, services) {
    foreach (const CSIPluginEndpoint& serviceEndpoint, info.endpoints()) {
      if (serviceEndpoint.csi_service() == service) {
        serviceEndpoints[service] = serviceEndpoint.endpoint();
        break;
      }
    }

    CHECK(serviceEndpoints.contains(service))
      << service << " not found for CSI plugin type '" << info.type()
      << "' and name '" << info.name() << "'";
  }
}


Future<Nothing> ServiceManagerProcess::recover()
{
  // For the unmanaged CSI plugin, we do not need to recover anything.
  if (!serviceEndpoints.empty()) {
    return Nothing();
  }

  CHECK(!serviceContainers.empty());

  return getContainers()
    .then(process::defer(self(), [=](
        const hashmap<ContainerID, Option<ContainerStatus>>& containers)
        -> Future<Nothing> {
      Try<list<string>> containerPaths =
        paths::getContainerPaths(rootDir, info.type(), info.name());

      if (containerPaths.isError()) {
        return Failure(
            "Failed to find service container paths for CSI plugin type '" +
            info.type() + "' and name '" + info.name() +
            "': " + containerPaths.error());
      }

      vector<Future<Nothing>> futures;

      foreach (const string& path, containerPaths.get()) {
        Try<paths::ContainerPath> containerPath =
          paths::parseContainerPath(rootDir, path);

        if (containerPath.isError()) {
          return Failure(
              "Failed to parse service container path '" + path +
              "': " + containerPath.error());
        }

        CHECK_EQ(info.type(), containerPath->type);
        CHECK_EQ(info.name(), containerPath->name);

        const ContainerID& containerId = containerPath->containerId;

        // NOTE: Since `GET_CONTAINERS` might return containers that are being
        // destroyed, to identify if the container is actually running, we check
        // if the `executor_pid` field is set as a workaround.
        bool isRunningContainer =
          containers.contains(containerId) &&
          containers.at(containerId).isSome() &&
          containers.at(containerId)->has_executor_pid();

        // Do not kill the up-to-date running controller or node container.
        if (serviceContainers.contains_value(containerId) &&
            isRunningContainer) {
          const string configPath = paths::getContainerInfoPath(
              rootDir, info.type(), info.name(), containerId);

          if (os::exists(configPath)) {
            Result<CSIPluginContainerInfo> config =
              slave::state::read<CSIPluginContainerInfo>(configPath);

            if (config.isError()) {
              return Failure(
                  "Failed to read service container config from '" +
                  configPath + "': " + config.error());
            }

            if (config.isSome() &&
                getContainerInfo(containerId) == config.get()) {
              continue;
            }
          }
        }

        LOG(INFO) << "Cleaning up plugin container '" << containerId << "'";

        // Otherwise, kill the container if it is running. We always wait for
        // the container to be destroyed before performing the cleanup even if
        // it is not killed here.
        Future<Nothing> cleanup = Nothing();
        if (containers.contains(containerId)) {
          if (isRunningContainer) {
            cleanup = killContainer(containerId);
          }
          cleanup = cleanup
            .then(process::defer(self(), &Self::waitContainer, containerId));
        }

        cleanup = cleanup
          .then(process::defer(self(), [=]() -> Future<Nothing> {
            Result<string> endpointDir =
              os::realpath(paths::getEndpointDirSymlinkPath(
                  rootDir, info.type(), info.name(), containerId));

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

      return process::collect(futures).then([] { return Nothing(); });
    }));
}


Future<string> ServiceManagerProcess::getServiceEndpoint(const Service& service)
{
  // For the unmanaged CSI plugin, get its endpoint from
  // `serviceEndpoints` directly.
  if (!serviceEndpoints.empty()) {
    if (serviceEndpoints.contains(service)) {
      return serviceEndpoints.at(service);
    } else {
      return Failure(
          stringify(service) + " not found for CSI plugin type '" +
          info.type() + "' and name '" + info.name() + "'");
    }
  }

  // For the managed CSI plugin, get its endpoint via its corresponding
  // standalone container ID.
  CHECK(!serviceContainers.empty());
  if (!serviceContainers.contains(service)) {
    return Failure(
        stringify(service) + " not found for CSI plugin type '" + info.type() +
        "' and name '" + info.name() + "'");
  }

  return getEndpoint(serviceContainers.at(service));
}


Future<string> ServiceManagerProcess::getApiVersion()
{
  if (apiVersion.isSome()) {
    return apiVersion.get();
  }

  // Ensure that the unmanaged CSI plugin has been probed (which does the API
  // version detection) before returning the API version.
  if (!serviceEndpoints.empty()) {
    return probeEndpoint(serviceEndpoints.begin()->second)
      .then(process::defer(self(), [=] { return CHECK_NOTNONE(apiVersion); }));
  }

  // For the managed CSI plugin, `probeEndpoint` will be internally called by
  // `getEndpoint` to do the API version detection.
  CHECK(!serviceContainers.empty());
  return getEndpoint(serviceContainers.begin()->second)
    .then(process::defer(self(), [=] { return CHECK_NOTNONE(apiVersion); }));
}


Option<CSIPluginContainerInfo> ServiceManagerProcess::getContainerInfo(
    const ContainerID& containerId)
{
  foreach (const auto& container, info.containers()) {
    if (getContainerId(info, containerPrefix, container) == containerId) {
      return container;
    }
  }

  return None();
}


Future<hashmap<ContainerID, Option<ContainerStatus>>>
ServiceManagerProcess::getContainers()
{
  agent::Call call;
  call.set_type(agent::Call::GET_CONTAINERS);
  call.mutable_get_containers()->set_show_nested(false);
  call.mutable_get_containers()->set_show_standalone(true);

  return http::post(
      agentUrl,
      headers,
      internal::serialize(contentType, internal::evolve(call)),
      stringify(contentType))
    .then(process::defer(self(), [this](const http::Response& httpResponse)
        -> Future<hashmap<ContainerID, Option<ContainerStatus>>> {
      if (httpResponse.status != http::OK().status) {
        return Failure(
            "Failed to get containers: Unexpected response '" +
            httpResponse.status + "' (" + httpResponse.body + ")");
      }

      Try<mesos::v1::agent::Response> v1Response =
        internal::deserialize<mesos::v1::agent::Response>(
            contentType, httpResponse.body);

      if (v1Response.isError()) {
        return Failure("Failed to get containers: " + v1Response.error());
      }

      hashmap<ContainerID, Option<ContainerStatus>> result;

      agent::Response response = internal::devolve(v1Response.get());
      foreach (const agent::Response::GetContainers::Container& container,
               response.get_containers().containers()) {
        // Container IDs of this CSI plugin must contain the given prefix. See
        // `LocalResourceProvider::principal` for details.
        if (!strings::startsWith(
                container.container_id().value(), containerPrefix)) {
          continue;
        }

        result.put(
            container.container_id(),
            container.has_container_status() ? container.container_status()
                                             : Option<ContainerStatus>::none());
      }

      return std::move(result);
    }));
}


Future<Nothing> ServiceManagerProcess::waitContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_CONTAINER);
  call.mutable_wait_container()->mutable_container_id()->CopyFrom(containerId);

  return http::post(
      agentUrl,
      headers,
      internal::serialize(contentType, internal::evolve(call)),
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


Future<Nothing> ServiceManagerProcess::killContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::KILL_CONTAINER);
  call.mutable_kill_container()->mutable_container_id()->CopyFrom(containerId);

  return http::post(
      agentUrl,
      headers,
      internal::serialize(contentType, internal::evolve(call)),
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


Future<Nothing> ServiceManagerProcess::waitEndpoint(const string& endpoint)
{
  CHECK(strings::startsWith(endpoint, "unix://"));
  const string endpointPath =
    strings::remove(endpoint, "unix://", strings::PREFIX);

  if (os::exists(endpointPath)) {
    return Nothing();
  }

  // Wait for the endpoint socket to appear until the timeout expires.
  Timeout timeout = Timeout::in(CSI_ENDPOINT_CREATION_TIMEOUT);

  return process::loop(
      [=]() -> Future<Nothing> {
        if (timeout.expired()) {
          return Failure("Timed out waiting for endpoint '" + endpoint + "'");
        }

        return process::after(Milliseconds(10));
      },
      [=](const Nothing&) -> ControlFlow<Nothing> {
        if (os::exists(endpointPath)) {
          return Break();
        }

        return Continue();
      });
}


Future<Nothing> ServiceManagerProcess::probeEndpoint(const string& endpoint)
{
  // Each probe function returns its API version if the probe is successful,
  // an error if the API version is implemented but the probe fails, or a `None`
  // if the API version is not implemented.
  static const hashmap<
      string,
      std::function<Future<Result<string>>(const string&, const Runtime&)>>
    probers = {
      {v0::API_VERSION,
       [](const string& endpoint, const Runtime& runtime) {
         LOG(INFO) << "Probing endpoint '" << endpoint << "' with CSI v0";

         return v0::Client(endpoint, runtime)
           .probe(v0::ProbeRequest())
           .then([](const v0::RPCResult<v0::ProbeResponse>& result) {
             return result.isError()
               ? (result.error().status.error_code() == grpc::UNIMPLEMENTED
                    ? Result<string>::none() : result.error())
               : v0::API_VERSION;
           });
       }},
      {v1::API_VERSION,
       [](const string& endpoint, const Runtime& runtime) {
         LOG(INFO) << "Probing endpoint '" << endpoint << "' with CSI v1";

         return v1::Client(endpoint, runtime).probe(v1::ProbeRequest())
           .then([](const v1::RPCResult<v1::ProbeResponse>& result) {
             // TODO(chhsiao): Retry when `result->ready` is false.
             return result.isError()
               ? (result.error().status.error_code() == grpc::UNIMPLEMENTED
                    ? Result<string>::none() : result.error())
               : v1::API_VERSION;
           });
       }},
    };

  ++metrics->csi_plugin_rpcs_pending;

  Future<Result<string>> probed;

  if (apiVersion.isSome()) {
    CHECK(probers.contains(apiVersion.get()));
    probed = probers.at(apiVersion.get())(endpoint, runtime);
  } else {
    probed = probers.at(v1::API_VERSION)(endpoint, runtime)
      .then(process::defer(self(), [=](const Result<string>& result) {
        return result.isNone()
          ? probers.at(v0::API_VERSION)(endpoint, runtime) : result;
      }));
  }

  return probed
    .then(process::defer(self(), [=](
        const Result<string>& result) -> Future<Nothing> {
      if (result.isError()) {
        return Failure(
            "Failed to probe endpoint '" + endpoint + "': " + result.error());
      }

      if (result.isNone()) {
        return Failure(
            "Failed to probe endpoint '" + endpoint + "': Unknown API version");
      }

      if (apiVersion.isNone()) {
        apiVersion = result.get();
      } else if (apiVersion != result.get()) {
        return Failure(
            "Failed to probe endpoint '" + endpoint +
            "': Inconsistent API version");
      }

      return Nothing();
    }))
    .onAny(process::defer(self(), [this](const Future<Nothing>& future) {
      // We only update the metrics after the whole detection loop is done so
      // it won't introduce much noise.
      --metrics->csi_plugin_rpcs_pending;
      if (future.isReady()) {
        ++metrics->csi_plugin_rpcs_finished;
      } else if (future.isDiscarded()) {
        ++metrics->csi_plugin_rpcs_cancelled;
      } else {
        ++metrics->csi_plugin_rpcs_failed;
      }
    }));
}


Future<string> ServiceManagerProcess::getEndpoint(
    const ContainerID& containerId)
{
  if (endpoints.contains(containerId)) {
    return endpoints.at(containerId)->future();
  }

  CHECK(!daemons.contains(containerId));

  Option<CSIPluginContainerInfo> config = getContainerInfo(containerId);
  CHECK_SOME(config);

  // We checkpoint the config first to keep track of the plugin container even
  // if we fail to create its container daemon.
  const string configPath =
    paths::getContainerInfoPath(rootDir, info.type(), info.name(), containerId);

  Try<Nothing> checkpoint = slave::state::checkpoint(configPath, config.get());
  if (checkpoint.isError()) {
    return Failure(
        "Failed to checkpoint plugin container config to '" + configPath +
        "': " + checkpoint.error());
  }

  CommandInfo commandInfo;

  if (config->has_command()) {
    commandInfo = config->command();
  }

  // Set the `CSI_ENDPOINT` environment variable.
  Try<string> endpointPath = paths::getEndpointSocketPath(
      rootDir, info.type(), info.name(), containerId);

  if (endpointPath.isError()) {
    return Failure(
        "Failed to resolve endpoint path for plugin container '" +
        stringify(containerId) + "': " + endpointPath.error());
  }

  const string endpoint = "unix://" + endpointPath.get();
  Environment::Variable* endpoint_ =
    commandInfo.mutable_environment()->add_variables();

  endpoint_->set_name("CSI_ENDPOINT");
  endpoint_->set_value(endpoint);

  // For some CSI Plugins (like NFS CSI plugin), their node service need
  // a node ID specified by container orchestrator, so here we expose agent
  // ID to the plugins, they can use that as the node ID.
  if (config->services().end() != std::find(
          config->services().begin(),
          config->services().end(),
          NODE_SERVICE)) {
    Environment::Variable* nodeId =
      commandInfo.mutable_environment()->add_variables();

    nodeId->set_name("MESOS_AGENT_ID");
    nodeId->set_value(stringify(agentId));
  }

  ContainerInfo containerInfo;

  if (config->has_container()) {
    containerInfo = config->container();
  } else {
    containerInfo.set_type(ContainerInfo::MESOS);
  }

  // Prepare a volume where the endpoint socket will be placed.
  const string endpointDir = Path(endpointPath.get()).dirname();
  Volume* endpointVolume = containerInfo.add_volumes();
  endpointVolume->set_mode(Volume::RW);
  endpointVolume->set_container_path(endpointDir);
  endpointVolume->set_host_path(endpointDir);

  // Prepare the directory where the mount points will be placed.
  const string mountRootDir =
    paths::getMountRootDir(rootDir, info.type(), info.name());

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

  CHECK(!endpoints.contains(containerId));
  endpoints[containerId].reset(new Promise<string>());

  Try<Owned<ContainerDaemon>> daemon = ContainerDaemon::create(
      agentUrl,
      authToken,
      containerId,
      std::move(commandInfo),
      config->resources(),
      std::move(containerInfo),
      std::function<Future<Nothing>()>(
          process::defer(self(), [=]() -> Future<Nothing> {
            LOG(INFO)
              << "Connecting to endpoint '" << endpoint
              << "' of CSI plugin container " << containerId;

            CHECK(endpoints.at(containerId)->associate(
                waitEndpoint(endpoint)
                  .then(process::defer(self(), &Self::probeEndpoint, endpoint))
                  .then([endpoint]() -> string { return endpoint; })));

            return endpoints.at(containerId)->future().then([] {
              return Nothing();
            });
          })),
      std::function<Future<Nothing>()>(
          process::defer(self(), [=]() -> Future<Nothing> {
            ++metrics->csi_plugin_container_terminations;

            endpoints.at(containerId)->discard();
            endpoints.at(containerId).reset(new Promise<string>());

            LOG(INFO)
              << "Disconnected from endpoint '" << endpoint
              << "' of CSI plugin container " << containerId;

            const string endpointPath =
              strings::remove(endpoint, "unix://", strings::PREFIX);

            if (os::exists(endpointPath)) {
              Try<Nothing> rm = os::rm(endpointPath);
              if (rm.isError()) {
                return Failure(
                    "Failed to remove endpoint socket '" + endpointPath +
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

  daemon.get()->wait()
    .recover(process::defer(self(), [this, containerId](
        const Future<Nothing>& future) {
      LOG(ERROR)
        << "Container daemon for '" << containerId << "' failed: "
        << (future.isFailed() ? future.failure() : "future discarded");

      // Fail or discard the corresponding endpoint promise if is has not been
      // associated by the post-start hook above yet.
      endpoints.at(containerId)->associate(
          future.then([]() -> string { UNREACHABLE(); }));

      return future;
    }));

  daemons.put(containerId, std::move(daemon.get()));

  return endpoints.at(containerId)->future();
}


ServiceManager::ServiceManager(
    const SlaveID& agentId,
    const http::URL& agentUrl,
    const string& rootDir,
    const CSIPluginInfo& info,
    const hashset<Service>& services,
    const string& containerPrefix,
    const Option<string>& authToken,
    const Runtime& runtime,
    Metrics* metrics)
  : process(new ServiceManagerProcess(
        agentId,
        agentUrl,
        rootDir,
        info,
        services,
        containerPrefix,
        authToken,
        runtime,
        metrics))
{
  process::spawn(CHECK_NOTNULL(process.get()));
  recovered = process::dispatch(process.get(), &ServiceManagerProcess::recover);
}


ServiceManager::ServiceManager(
    const CSIPluginInfo& info,
    const hashset<Service>& services,
    const process::grpc::client::Runtime& runtime,
    Metrics* metrics)
  : process(new ServiceManagerProcess(
        info,
        services,
        runtime,
        metrics))
{
  process::spawn(CHECK_NOTNULL(process.get()));
  recovered = process::dispatch(process.get(), &ServiceManagerProcess::recover);
}


ServiceManager::~ServiceManager()
{
  recovered.discard();
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> ServiceManager::recover()
{
  return recovered;
}


Future<string> ServiceManager::getServiceEndpoint(const Service& service)
{
  return recovered
    .then(process::defer(
        process.get(), &ServiceManagerProcess::getServiceEndpoint, service));
}


Future<string> ServiceManager::getApiVersion()
{
  return recovered
    .then(process::defer(process.get(), &ServiceManagerProcess::getApiVersion));
}

} // namespace csi {
} // namespace mesos {
