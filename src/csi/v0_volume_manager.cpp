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

#include "csi/v0_volume_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <list>

#include <mesos/secret/resolver.hpp>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "csi/constants.hpp"
#include "csi/paths.hpp"
#include "csi/v0_client.hpp"
#include "csi/v0_utils.hpp"
#include "csi/v0_volume_manager_process.hpp"

#include "slave/state.hpp"

namespace http = process::http;
namespace slave = mesos::internal::slave;

using std::list;
using std::string;
using std::vector;

using google::protobuf::Map;

using mesos::csi::state::VolumeState;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::Owned;
using process::ProcessBase;

using process::grpc::StatusError;

using process::grpc::client::Runtime;

namespace mesos{
namespace csi {
namespace v0 {

VolumeManagerProcess::VolumeManagerProcess(
    const string& _rootDir,
    const CSIPluginInfo& _info,
    const hashset<Service> _services,
    const Runtime& _runtime,
    ServiceManager* _serviceManager,
    Metrics* _metrics,
    SecretResolver* _secretResolver)
  : ProcessBase(process::ID::generate("csi-v0-volume-manager")),
    rootDir(_rootDir),
    info(_info),
    services(_services),
    runtime(_runtime),
    serviceManager(_serviceManager),
    metrics(_metrics),
    secretResolver(_secretResolver),
    mountRootDir(info.has_target_path_root()
      ? info.target_path_root()
      : paths::getMountRootDir(rootDir, info.type(), info.name()))
{
  // This should have been validated in `VolumeManager::create`.
  CHECK(!services.empty())
    << "Must specify at least one service for CSI plugin type '" << info.type()
    << "' and name '" << info.name() << "'";
}


Future<Nothing> VolumeManagerProcess::recover()
{
  Try<string> bootId_ = os::bootId();
  if (bootId_.isError()) {
    return Failure("Failed to get boot ID: " + bootId_.error());
  }

  bootId = bootId_.get();

  return prepareServices()
    .then(process::defer(self(), [this]() -> Future<Nothing> {
      // Recover the states of CSI volumes.
      Try<list<string>> volumePaths =
        paths::getVolumePaths(rootDir, info.type(), info.name());

      if (volumePaths.isError()) {
        return Failure(
            "Failed to find volumes for CSI plugin type '" + info.type() +
            "' and name '" + info.name() + "': " + volumePaths.error());
      }

      vector<Future<Nothing>> futures;

      foreach (const string& path, volumePaths.get()) {
        Try<paths::VolumePath> volumePath =
          paths::parseVolumePath(rootDir, path);

        if (volumePath.isError()) {
          return Failure(
              "Failed to parse volume path '" + path +
              "': " + volumePath.error());
        }

        CHECK_EQ(info.type(), volumePath->type);
        CHECK_EQ(info.name(), volumePath->name);

        const string& volumeId = volumePath->volumeId;
        const string statePath = paths::getVolumeStatePath(
            rootDir, info.type(), info.name(), volumeId);

        if (!os::exists(statePath)) {
          continue;
        }

        Result<VolumeState> volumeState =
          slave::state::read<VolumeState>(statePath);

        if (volumeState.isError()) {
          return Failure(
              "Failed to read volume state from '" + statePath +
              "': " + volumeState.error());
        }

        if (volumeState.isNone()) {
          continue;
        }

        volumes.put(volumeId, std::move(volumeState.get()));
        VolumeData& volume = volumes.at(volumeId);

        if (!VolumeState::State_IsValid(volume.state.state())) {
          return Failure("Volume '" + volumeId + "' is in INVALID state");
        }

        // First, if there is a node reboot after the volume is made
        // publishable, it should be reset to `NODE_READY`.
        switch (volume.state.state()) {
          case VolumeState::CREATED:
          case VolumeState::NODE_READY:
          case VolumeState::CONTROLLER_PUBLISH:
          case VolumeState::CONTROLLER_UNPUBLISH:
          case VolumeState::NODE_STAGE: {
            break;
          }
          case VolumeState::VOL_READY:
          case VolumeState::PUBLISHED:
          case VolumeState::NODE_UNSTAGE:
          case VolumeState::NODE_PUBLISH:
          case VolumeState::NODE_UNPUBLISH: {
            if (bootId != volume.state.boot_id()) {
              // Since this is a no-op, no need to checkpoint here.
              volume.state.set_state(VolumeState::NODE_READY);
              volume.state.clear_boot_id();
            }

            break;
          }
          case VolumeState::UNKNOWN: {
            return Failure("Volume '" + volumeId + "' is in UNKNOWN state");
          }

          // NOTE: We avoid using a default clause for the following values in
          // proto3's open enum to enable the compiler to detect missing enum
          // cases for us. See: https://github.com/google/protobuf/issues/3917
          case google::protobuf::kint32min:
          case google::protobuf::kint32max: {
            UNREACHABLE();
          }
        }

        // Second, if the volume has been used by a container before recovery,
        // we have to bring the volume back to `PUBLISHED` so data can be
        // cleaned up synchronously when needed.
        if (volume.state.node_publish_required()) {
          futures.push_back(publishVolume(volumeId));
        }
      }

      // Garbage collect leftover mount paths that were failed to remove before.
      Try<list<string>> mountPaths = paths::getMountPaths(mountRootDir);
      if (mountPaths.isError()) {
        // TODO(chhsiao): This could indicate that something is seriously wrong.
        // To help debugging the problem, we should surface the error via
        // MESOS-8745.
        return Failure(
            "Failed to find mount paths for CSI plugin type '" + info.type() +
            "' and name '" + info.name() + "': " + mountPaths.error());
      }

      foreach (const string& path, mountPaths.get()) {
        Try<string> volumeId = paths::parseMountPath(mountRootDir, path);
        if (volumeId.isError()) {
          return Failure(
              "Failed to parse mount path '" + path + "': " + volumeId.error());
        }

        if (!volumes.contains(volumeId.get())) {
          garbageCollectMountPath(volumeId.get());
        }
      }

      return process::collect(futures).then([] { return Nothing(); });
    }));
}


Future<vector<VolumeInfo>> VolumeManagerProcess::listVolumes()
{
  if (!controllerCapabilities->listVolumes) {
    return vector<VolumeInfo>();
  }

  // TODO(chhsiao): Set the max entries and use a loop to do multiple
  // `ListVolumes` calls.
  return call(CONTROLLER_SERVICE, &Client::listVolumes, ListVolumesRequest())
    .then(process::defer(self(), [](const ListVolumesResponse& response) {
      vector<VolumeInfo> result;
      foreach (const auto& entry, response.entries()) {
        result.push_back(VolumeInfo{Bytes(entry.volume().capacity_bytes()),
                                    entry.volume().id(),
                                    entry.volume().attributes()});
      }

      return result;
    }));
}


Future<Bytes> VolumeManagerProcess::getCapacity(
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  if (!controllerCapabilities->getCapacity) {
    return Bytes(0);
  }

  GetCapacityRequest request;
  *request.add_volume_capabilities() = evolve(capability);
  *request.mutable_parameters() = parameters;

  return call(CONTROLLER_SERVICE, &Client::getCapacity, std::move(request))
    .then([](const GetCapacityResponse& response) {
      return Bytes(response.available_capacity());
    });
}


Future<VolumeInfo> VolumeManagerProcess::createVolume(
    const string& name,
    const Bytes& capacity,
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  if (!controllerCapabilities->createDeleteVolume) {
    return Failure(
        "CREATE_DELETE_VOLUME controller capability is not supported for CSI "
        "plugin type '" + info.type() + "' and name '" + info.name());
  }

  LOG(INFO) << "Creating volume with name '" << name << "'";

  CreateVolumeRequest request;
  request.set_name(name);
  request.mutable_capacity_range()->set_required_bytes(capacity.bytes());
  request.mutable_capacity_range()->set_limit_bytes(capacity.bytes());
  *request.add_volume_capabilities() = evolve(capability);
  *request.mutable_parameters() = parameters;

  // We retry the `CreateVolume` call for MESOS-9517.
  return call(
      CONTROLLER_SERVICE, &Client::createVolume, std::move(request), true)
    .then(process::defer(self(), [=](
        const CreateVolumeResponse& response) -> Future<VolumeInfo> {
      const string& volumeId = response.volume().id();

      // NOTE: If the volume is already tracked, there might already be
      // operations running in its sequence. Since this continuation runs
      // outside the sequence, we fail the call here to avoid any race issue.
      // This also means that this call is not idempotent.
      if (volumes.contains(volumeId)) {
        return Failure("Volume with name '" + name + "' already exists");
      }

      VolumeState volumeState;
      volumeState.set_state(VolumeState::CREATED);
      *volumeState.mutable_volume_capability() = capability;
      *volumeState.mutable_parameters() = parameters;
      *volumeState.mutable_volume_context() = response.volume().attributes();

      volumes.put(volumeId, std::move(volumeState));
      checkpointVolumeState(volumeId);

      return VolumeInfo{capacity, volumeId, response.volume().attributes()};
    }));
}


Future<Option<Error>> VolumeManagerProcess::validateVolume(
    const VolumeInfo& volumeInfo,
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  // If the volume has been checkpointed, the validation succeeds only if the
  // capability and parameters of the specified profile are the same as those in
  // the checkpoint.
  if (volumes.contains(volumeInfo.id)) {
    const VolumeState& volumeState = volumes.at(volumeInfo.id).state;

    if (volumeState.volume_capability() != capability) {
      return Some(
          Error("Mismatched capability for volume '" + volumeInfo.id + "'"));
    }

    if (volumeState.parameters() != parameters) {
      return Some(
          Error("Mismatched parameters for volume '" + volumeInfo.id + "'"));
    }

    return None();
  }

  if (!parameters.empty()) {
    LOG(WARNING)
      << "Validating volumes against parameters is not supported in CSI v0";
  }

  LOG(INFO) << "Validating volume '" << volumeInfo.id << "'";

  ValidateVolumeCapabilitiesRequest request;
  request.set_volume_id(volumeInfo.id);
  *request.add_volume_capabilities() = evolve(capability);
  *request.mutable_volume_attributes() = volumeInfo.context;

  return call(
      CONTROLLER_SERVICE,
      &Client::validateVolumeCapabilities,
      std::move(request))
    .then(process::defer(self(), [=](
        const ValidateVolumeCapabilitiesResponse& response)
        -> Future<Option<Error>> {
      if (!response.supported()) {
        return Error(
            "Unsupported volume capability for volume '" + volumeInfo.id +
            "': " + response.message());
      }

      // NOTE: If the volume is already tracked, there might already be
      // operations running in its sequence. Since this continuation runs
      // outside the sequence, we fail the call here to avoid any race issue.
      // This also means that this call is not idempotent.
      if (volumes.contains(volumeInfo.id)) {
        return Failure("Volume '" + volumeInfo.id + "' already validated");
      }

      VolumeState volumeState;
      volumeState.set_state(VolumeState::CREATED);
      *volumeState.mutable_volume_capability() = capability;
      *volumeState.mutable_parameters() = parameters;
      *volumeState.mutable_volume_context() = volumeInfo.context;

      volumes.put(volumeInfo.id, std::move(volumeState));
      checkpointVolumeState(volumeInfo.id);

      return None();
    }));
}


Future<bool> VolumeManagerProcess::deleteVolume(const string& volumeId)
{
  if (!volumes.contains(volumeId)) {
    return __deleteVolume(volumeId);
  }

  VolumeData& volume = volumes.at(volumeId);

  LOG(INFO) << "Deleting volume '" << volumeId << "' in "
            << volume.state.state() << " state";

  // Volume deletion is sequentialized with other operations on the same volume
  // to avoid races.
  return volume.sequence->add(std::function<Future<bool>()>(
      process::defer(self(), &Self::_deleteVolume, volumeId)));
}


Future<Nothing> VolumeManagerProcess::attachVolume(const string& volumeId)
{
  if (!volumes.contains(volumeId)) {
    return Failure("Cannot attach unknown volume '" + volumeId + "'");
  }

  VolumeData& volume = volumes.at(volumeId);

  LOG(INFO) << "Attaching volume '" << volumeId << "' in "
            << volume.state.state() << " state";

  // Volume attaching is serialized with other operations on the same volume to
  // avoid races.
  return volume.sequence->add(std::function<Future<Nothing>()>(
      process::defer(self(), &Self::_attachVolume, volumeId)));
}


Future<Nothing> VolumeManagerProcess::detachVolume(const string& volumeId)
{
  if (!volumes.contains(volumeId)) {
    return Failure("Cannot detach unknown volume '" + volumeId + "'");
  }

  VolumeData& volume = volumes.at(volumeId);

  LOG(INFO) << "Detaching volume '" << volumeId << "' in "
            << volume.state.state() << " state";

  // Volume detaching is serialized with other operations on the same volume to
  // avoid races.
  return volume.sequence->add(std::function<Future<Nothing>()>(
      process::defer(self(), &Self::_detachVolume, volumeId)));
}


Future<Nothing> VolumeManagerProcess::publishVolume(
    const string& volumeId,
    const Option<VolumeState>& volumeState)
{
  if (volumeState.isSome()) {
    if (!volumeState->pre_provisioned()) {
      return Failure(
          "Cannot specify volume state when publishing a volume unless that"
          " volume is pre-provisioned");
    }

    if (volumeState->state() != VolumeState::VOL_READY &&
        volumeState->state() != VolumeState::NODE_READY) {
      return Failure(
          "Cannot specify volume state when publishing a volume unless that"
          " volume is in either the VOL_READY or NODE_READY state");
    }

    // This must be an untracked volume. Track it now before we continue.
    volumes.put(volumeId, VolumeState(volumeState.get()));
    checkpointVolumeState(volumeId);
  }

  if (!volumes.contains(volumeId)) {
    return Failure("Cannot publish unknown volume '" + volumeId + "'");
  }

  VolumeData& volume = volumes.at(volumeId);

  LOG(INFO) << "Publishing volume '" << volumeId << "' in "
            << volume.state.state() << " state";

  // Volume publishing is serialized with other operations on the same volume to
  // avoid races.
  return volume.sequence->add(std::function<Future<Nothing>()>(
      process::defer(self(), &Self::_publishVolume, volumeId)));
}


Future<Nothing> VolumeManagerProcess::unpublishVolume(const string& volumeId)
{
  if (!volumes.contains(volumeId)) {
    LOG(WARNING) << "Ignoring unpublish request for unknown volume '"
                 << volumeId << "'";

    return Nothing();
  }

  VolumeData& volume = volumes.at(volumeId);

  LOG(INFO) << "Unpublishing volume '" << volumeId << "' in "
            << volume.state.state() << " state";

  // Volume unpublishing is serialized with other operations on the same volume
  // to avoid races.
  return volume.sequence->add(std::function<Future<Nothing>()>(
      process::defer(self(), &Self::_unpublishVolume, volumeId)));
}


template <typename Request, typename Response>
Future<Response> VolumeManagerProcess::call(
    const Service& service,
    Future<RPCResult<Response>> (Client::*rpc)(Request),
    const Request& request,
    const bool retry) // Made immutable in the following mutable lambda.
{
  Duration maxBackoff = DEFAULT_RPC_RETRY_BACKOFF_FACTOR;

  return process::loop(
      self(),
      [=] {
        // Make the call to the latest service endpoint.
        return serviceManager->getServiceEndpoint(service)
          .then(process::defer(
              self(),
              &VolumeManagerProcess::_call<Request, Response>,
              lambda::_1,
              rpc,
              request));
      },
      [=](const RPCResult<Response>& result) mutable
          -> Future<ControlFlow<Response>> {
        Option<Duration> backoff = retry
          ? maxBackoff * (static_cast<double>(os::random()) / RAND_MAX)
          : Option<Duration>::none();

        maxBackoff = std::min(maxBackoff * 2, DEFAULT_RPC_RETRY_INTERVAL_MAX);

        // We dispatch `__call` for testing purpose.
        return process::dispatch(
            self(), &VolumeManagerProcess::__call<Response>, result, backoff);
      });
}


template <typename Request, typename Response>
Future<RPCResult<Response>> VolumeManagerProcess::_call(
    const string& endpoint,
    Future<RPCResult<Response>> (Client::*rpc)(Request),
    const Request& request)
{
  ++metrics->csi_plugin_rpcs_pending;

  return (Client(endpoint, runtime).*rpc)(request).onAny(
      process::defer(self(), [=](const Future<RPCResult<Response>>& future) {
        --metrics->csi_plugin_rpcs_pending;
        if (future.isReady() && future->isSome()) {
          ++metrics->csi_plugin_rpcs_finished;
        } else if (future.isDiscarded()) {
          ++metrics->csi_plugin_rpcs_cancelled;
        } else {
          ++metrics->csi_plugin_rpcs_failed;
        }
      }));
}


template <typename Response>
Future<ControlFlow<Response>> VolumeManagerProcess::__call(
    const RPCResult<Response>& result, const Option<Duration>& backoff)
{
  if (result.isSome()) {
    return Break(result.get());
  }

  if (backoff.isNone()) {
    return Failure(result.error());
  }

  // See the link below for retryable status codes:
  // https://grpc.io/grpc/cpp/namespacegrpc.html#aff1730578c90160528f6a8d67ef5c43b // NOLINT
  switch (result.error().status.error_code()) {
    case grpc::DEADLINE_EXCEEDED:
    case grpc::UNAVAILABLE: {
      LOG(ERROR) << "Received '" << result.error() << "' while expecting "
                 << Response::descriptor()->name() << ". Retrying in "
                 << backoff.get();

      return process::after(backoff.get())
        .then([]() -> Future<ControlFlow<Response>> { return Continue(); });
    }
    case grpc::CANCELLED:
    case grpc::UNKNOWN:
    case grpc::INVALID_ARGUMENT:
    case grpc::NOT_FOUND:
    case grpc::ALREADY_EXISTS:
    case grpc::PERMISSION_DENIED:
    case grpc::UNAUTHENTICATED:
    case grpc::RESOURCE_EXHAUSTED:
    case grpc::FAILED_PRECONDITION:
    case grpc::ABORTED:
    case grpc::OUT_OF_RANGE:
    case grpc::UNIMPLEMENTED:
    case grpc::INTERNAL:
    case grpc::DATA_LOSS: {
      return Failure(result.error());
    }
    case grpc::OK:
    case grpc::DO_NOT_USE: {
      UNREACHABLE();
    }
  }

  UNREACHABLE();
}


Future<Nothing> VolumeManagerProcess::prepareServices()
{
  CHECK(!services.empty());

  // Get the plugin capabilities.
  return call(
      *services.begin(),
      &Client::getPluginCapabilities,
      GetPluginCapabilitiesRequest())
    .then(process::defer(self(), [=](
        const GetPluginCapabilitiesResponse& response) -> Future<Nothing> {
      pluginCapabilities = response.capabilities();

      if (services.contains(CONTROLLER_SERVICE) &&
          !pluginCapabilities->controllerService) {
        return Failure(
            "CONTROLLER_SERVICE plugin capability is not supported for CSI "
            "plugin type '" + info.type() + "' and name '" + info.name() + "'");
      }

      return Nothing();
    }))
    // Check if all services have consistent plugin infos.
    .then(process::defer(self(), [this] {
      vector<Future<GetPluginInfoResponse>> futures;
      foreach (const Service& service, services) {
        futures.push_back(call(
            service, &Client::getPluginInfo, GetPluginInfoRequest())
          .onReady([service](const GetPluginInfoResponse& response) {
            LOG(INFO) << service << " loaded: " << stringify(response);
          }));
      }

      return process::collect(futures)
        .then([](const vector<GetPluginInfoResponse>& pluginInfos) {
          for (size_t i = 1; i < pluginInfos.size(); ++i) {
            if (pluginInfos[i].name() != pluginInfos[0].name() ||
                pluginInfos[i].vendor_version() !=
                  pluginInfos[0].vendor_version()) {
              LOG(WARNING) << "Inconsistent plugin services. Please check with "
                              "the plugin vendor to ensure compatibility.";
            }
          }

          return Nothing();
        });
    }))
    // Get the controller capabilities.
    .then(process::defer(self(), [this]() -> Future<Nothing> {
      if (!services.contains(CONTROLLER_SERVICE)) {
        controllerCapabilities = ControllerCapabilities();
        return Nothing();
      }

      return call(
          CONTROLLER_SERVICE,
          &Client::controllerGetCapabilities,
          ControllerGetCapabilitiesRequest())
        .then(process::defer(self(), [this](
            const ControllerGetCapabilitiesResponse& response) {
          controllerCapabilities = response.capabilities();
          return Nothing();
        }));
    }))
    // Get the node capabilities and ID.
    .then(process::defer(self(), [this]() -> Future<Nothing> {
      if (!services.contains(NODE_SERVICE)) {
        nodeCapabilities = NodeCapabilities();
        return Nothing();
      }

      return call(
          NODE_SERVICE,
          &Client::nodeGetCapabilities,
          NodeGetCapabilitiesRequest())
        .then(process::defer(self(), [this](
            const NodeGetCapabilitiesResponse& response) -> Future<Nothing> {
          nodeCapabilities = response.capabilities();

          if (controllerCapabilities->publishUnpublishVolume) {
            return call(NODE_SERVICE, &Client::nodeGetId, NodeGetIdRequest())
              .then(process::defer(self(), [this](
                  const NodeGetIdResponse& response) {
                nodeId = response.node_id();
                return Nothing();
              }));
          }

          return Nothing();
        }));
    }));
}


Future<bool> VolumeManagerProcess::_deleteVolume(const std::string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.node_publish_required()) {
    CHECK_EQ(VolumeState::PUBLISHED, volumeState.state());

    const string targetPath = paths::getMountTargetPath(mountRootDir, volumeId);

    // NOTE: Normally the volume should have been cleaned up. However this may
    // not be true for preprovisioned volumes (e.g., leftover from a previous
    // resource provider instance). To prevent data leakage in such cases, we
    // clean up the data (but not the target path) here.
    Try<Nothing> rmdir = os::rmdir(targetPath, true, false);
    if (rmdir.isError()) {
      return Failure(
          "Failed to clean up volume '" + volumeId + "': " + rmdir.error());
    }

    volumeState.set_node_publish_required(false);
    checkpointVolumeState(volumeId);
  }

  if (volumeState.state() != VolumeState::CREATED) {
    // Retry after transitioning the volume to `CREATED` state.
    return _detachVolume(volumeId)
      .then(process::defer(self(), &Self::_deleteVolume, volumeId));
  }

  // NOTE: The last asynchronous continuation, which is supposed to be run in
  // the volume's sequence, would cause the sequence to be destructed, which
  // would in turn discard the returned future. However, since the continuation
  // would have already been run, the returned future will become ready, making
  // the future returned by the sequence ready as well.
  return __deleteVolume(volumeId)
    .then(process::defer(self(), [this, volumeId](bool deleted) {
      removeVolume(volumeId);

      return deleted;
    }));
}


Future<bool> VolumeManagerProcess::__deleteVolume(
    const string& volumeId)
{
  if (!controllerCapabilities->createDeleteVolume) {
    return false;
  }

  LOG(INFO) << "Calling '/csi.v0.Controller/DeleteVolume' for volume '"
            << volumeId << "'";

  DeleteVolumeRequest request;
  request.set_volume_id(volumeId);

  // We retry the `DeleteVolume` call for MESOS-9517.
  return call(
      CONTROLLER_SERVICE, &Client::deleteVolume, std::move(request), true)
    .then([] { return true; });
}


Future<Nothing> VolumeManagerProcess::_attachVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::NODE_READY) {
    return Nothing();
  }

  if (volumeState.state() != VolumeState::CREATED &&
      volumeState.state() != VolumeState::CONTROLLER_PUBLISH &&
      volumeState.state() != VolumeState::CONTROLLER_UNPUBLISH) {
    return Failure(
        "Cannot attach volume '" + volumeId + "' in " +
        stringify(volumeState.state()) + " state");
  }

  if (!controllerCapabilities->publishUnpublishVolume) {
    // Since this is a no-op, no need to checkpoint here.
    volumeState.set_state(VolumeState::NODE_READY);
    return Nothing();
  }

  // A previously failed `ControllerUnpublishVolume` call can be recovered
  // through an extra `ControllerUnpublishVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#controllerunpublishvolume // NOLINT
  if (volumeState.state() == VolumeState::CONTROLLER_UNPUBLISH) {
    // Retry after recovering the volume to `CREATED` state.
    return _detachVolume(volumeId)
      .then(process::defer(self(), &Self::_attachVolume, volumeId));
  }

  if (volumeState.state() == VolumeState::CREATED) {
    volumeState.set_state(VolumeState::CONTROLLER_PUBLISH);
    checkpointVolumeState(volumeId);
  }

  LOG(INFO)
    << "Calling '/csi.v0.Controller/ControllerPublishVolume' for volume '"
    << volumeId << "'";

  ControllerPublishVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_node_id(CHECK_NOTNONE(nodeId));
  *request.mutable_volume_capability() =
    evolve(volumeState.volume_capability());
  request.set_readonly(volumeState.readonly());
  *request.mutable_volume_attributes() = volumeState.volume_context();

  return call(
      CONTROLLER_SERVICE, &Client::controllerPublishVolume, std::move(request))
    .then(process::defer(self(), [this, volumeId](
        const ControllerPublishVolumeResponse& response) {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::NODE_READY);
      *volumeState.mutable_publish_context() = response.publish_info();

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> VolumeManagerProcess::_detachVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::CREATED) {
    return Nothing();
  }

  if (volumeState.state() != VolumeState::NODE_READY &&
      volumeState.state() != VolumeState::CONTROLLER_PUBLISH &&
      volumeState.state() != VolumeState::CONTROLLER_UNPUBLISH) {
    // Retry after transitioning the volume to `CREATED` state.
    return _unpublishVolume(volumeId)
      .then(process::defer(self(), &Self::_detachVolume, volumeId));
  }

  if (!controllerCapabilities->publishUnpublishVolume) {
    // Since this is a no-op, no need to checkpoint here.
    volumeState.set_state(VolumeState::CREATED);
    return Nothing();
  }

  // A previously failed `ControllerPublishVolume` call can be recovered through
  // the current `ControllerUnpublishVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#controllerpublishvolume // NOLINT
  if (volumeState.state() == VolumeState::NODE_READY ||
      volumeState.state() == VolumeState::CONTROLLER_PUBLISH) {
    volumeState.set_state(VolumeState::CONTROLLER_UNPUBLISH);
    checkpointVolumeState(volumeId);
  }

  LOG(INFO)
    << "Calling '/csi.v0.Controller/ControllerUnpublishVolume' for volume '"
    << volumeId << "'";

  ControllerUnpublishVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_node_id(CHECK_NOTNONE(nodeId));

  return call(
      CONTROLLER_SERVICE,
      &Client::controllerUnpublishVolume,
      std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::CREATED);
      volumeState.mutable_publish_context()->clear();

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> VolumeManagerProcess::_publishVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::PUBLISHED) {
    CHECK(volumeState.node_publish_required());
    return Nothing();
  }

  if (volumeState.state() != VolumeState::VOL_READY &&
      volumeState.state() != VolumeState::NODE_PUBLISH &&
      volumeState.state() != VolumeState::NODE_UNPUBLISH) {
    // Retry after transitioning the volume to `VOL_READY` state.
    return __publishVolume(volumeId)
      .then(process::defer(self(), &Self::_publishVolume, volumeId));
  }

  // A previously failed `NodeUnpublishVolume` call can be recovered through an
  // extra `NodeUnpublishVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodeunpublishvolume // NOLINT
  if (volumeState.state() == VolumeState::NODE_UNPUBLISH) {
    // Retry after recovering the volume to `VOL_READY` state.
    return __unpublishVolume(volumeId)
      .then(process::defer(self(), &Self::_publishVolume, volumeId));
  }

  const string targetPath = paths::getMountTargetPath(mountRootDir, volumeId);

  // NOTE: The target path will be cleaned up during volume removal.
  Try<Nothing> mkdir = os::mkdir(targetPath);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create mount target path '" + targetPath +
        "': " + mkdir.error());
  }

  if (volumeState.state() == VolumeState::VOL_READY) {
    volumeState.set_state(VolumeState::NODE_PUBLISH);
    checkpointVolumeState(volumeId);
  }

  LOG(INFO) << "Calling '/csi.v0.Node/NodePublishVolume' for volume '"
            << volumeId << "'";

  NodePublishVolumeRequest request;
  request.set_volume_id(volumeId);
  *request.mutable_publish_info() = volumeState.publish_context();
  request.set_target_path(targetPath);
  *request.mutable_volume_capability() =
    evolve(volumeState.volume_capability());
  request.set_readonly(volumeState.readonly());
  *request.mutable_volume_attributes() = volumeState.volume_context();

  if (nodeCapabilities->stageUnstageVolume) {
    const string stagingPath = paths::getMountStagingPath(
        mountRootDir, volumeId);

    CHECK(os::exists(stagingPath));
    request.set_staging_target_path(stagingPath);
  }

  Future<NodePublishVolumeResponse> rpcResult;

  if (!volumeState.node_publish_secrets().empty()) {
    rpcResult = resolveSecrets(volumeState.node_publish_secrets())
      .then(process::defer(
          self(),
          [this, request](const Map<string, string>& secrets) {
            NodePublishVolumeRequest request_(request);
            *request_.mutable_node_publish_secrets() = secrets;

            return call(
                NODE_SERVICE,
                &Client::nodePublishVolume,
                std::move(request_));
          }));
  } else {
    rpcResult =
      call(NODE_SERVICE, &Client::nodePublishVolume, std::move(request));
  }

  return rpcResult
    .then(process::defer(self(), [this, volumeId, targetPath]()
        -> Future<Nothing> {
      if (!os::exists(targetPath)) {
        return Failure("Target path '" + targetPath + "' not created");
      }

      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;

      volumeState.set_state(VolumeState::PUBLISHED);

      // NOTE: This is the first time a container is going to consume the
      // persistent volume, so the `node_publish_required` field is set to
      // indicate that this volume must remain published so it can be
      // synchronously cleaned up when the persistent volume is destroyed.
      volumeState.set_node_publish_required(true);

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> VolumeManagerProcess::__publishVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::VOL_READY) {
    CHECK(!volumeState.boot_id().empty());
    return Nothing();
  }

  if (volumeState.state() != VolumeState::NODE_READY &&
      volumeState.state() != VolumeState::NODE_STAGE &&
      volumeState.state() != VolumeState::NODE_UNSTAGE) {
    // Retry after transitioning the volume to `NODE_READY` state.
    return _attachVolume(volumeId)
      .then(process::defer(self(), &Self::__publishVolume, volumeId));
  }

  if (!nodeCapabilities->stageUnstageVolume) {
    // Since this is a no-op, no need to checkpoint here.
    volumeState.set_state(VolumeState::VOL_READY);
    volumeState.set_boot_id(CHECK_NOTNONE(bootId));
    return Nothing();
  }

  // A previously failed `NodeUnstageVolume` call can be recovered through an
  // extra `NodeUnstageVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodeunstagevolume // NOLINT
  if (volumeState.state() == VolumeState::NODE_UNSTAGE) {
    // Retry after recovering the volume to `NODE_READY` state.
    return _unpublishVolume(volumeId)
      .then(process::defer(self(), &Self::__publishVolume, volumeId));
  }

  const string stagingPath = paths::getMountStagingPath(mountRootDir, volumeId);

  // NOTE: The staging path will be cleaned up in during volume removal.
  Try<Nothing> mkdir = os::mkdir(stagingPath);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create mount staging path '" + stagingPath +
        "': " + mkdir.error());
  }

  if (volumeState.state() == VolumeState::NODE_READY) {
    volumeState.set_state(VolumeState::NODE_STAGE);
    checkpointVolumeState(volumeId);
  }

  LOG(INFO) << "Calling '/csi.v0.Node/NodeStageVolume' for volume '" << volumeId
            << "'";

  NodeStageVolumeRequest request;
  request.set_volume_id(volumeId);
  *request.mutable_publish_info() = volumeState.publish_context();
  request.set_staging_target_path(stagingPath);
  *request.mutable_volume_capability() =
    evolve(volumeState.volume_capability());
  *request.mutable_volume_attributes() = volumeState.volume_context();

  Future<NodeStageVolumeResponse> rpcResult;

  if (!volumeState.node_stage_secrets().empty()) {
    rpcResult = resolveSecrets(volumeState.node_stage_secrets())
      .then(process::defer(self(), [=](const Map<string, string>& secrets) {
        NodeStageVolumeRequest request_(request);
        *request_.mutable_node_stage_secrets() = secrets;

        return call(
            NODE_SERVICE,
            &Client::nodeStageVolume,
            std::move(request_));
      }));
  } else {
    rpcResult =
      call(NODE_SERVICE, &Client::nodeStageVolume, std::move(request));
  }

  return rpcResult
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::VOL_READY);
      volumeState.set_boot_id(CHECK_NOTNONE(bootId));

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> VolumeManagerProcess::_unpublishVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::NODE_READY) {
    CHECK(volumeState.boot_id().empty());

    if (volumeState.pre_provisioned()) {
      // Since this volume was pre-provisioned, it has reached the end of its
      // lifecycle. Remove it now.
      removeVolume(volumeId);
    }

    return Nothing();
  }

  if (volumeState.state() != VolumeState::VOL_READY &&
      volumeState.state() != VolumeState::NODE_STAGE &&
      volumeState.state() != VolumeState::NODE_UNSTAGE) {
    // Retry after transitioning the volume to `VOL_READY` state.
    return __unpublishVolume(volumeId)
      .then(process::defer(self(), &Self::_unpublishVolume, volumeId));
  }

  if (!nodeCapabilities->stageUnstageVolume) {
    if (volumeState.pre_provisioned()) {
      // Since this volume was pre-provisioned, it has reached the end of its
      // lifecycle. Remove it now.
      removeVolume(volumeId);
    } else {
      // Since this is a no-op, no need to checkpoint here.
      volumeState.set_state(VolumeState::NODE_READY);
      volumeState.clear_boot_id();
    }

    return Nothing();
  }

  // A previously failed `NodeStageVolume` call can be recovered through the
  // current `NodeUnstageVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodestagevolume // NOLINT
  if (volumeState.state() == VolumeState::VOL_READY ||
      volumeState.state() == VolumeState::NODE_STAGE) {
    volumeState.set_state(VolumeState::NODE_UNSTAGE);
    checkpointVolumeState(volumeId);
  }

  const string stagingPath = paths::getMountStagingPath(mountRootDir, volumeId);

  CHECK(os::exists(stagingPath));

  LOG(INFO) << "Calling '/csi.v0.Node/NodeUnstageVolume' for volume '"
            << volumeId << "'";

  NodeUnstageVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_staging_target_path(stagingPath);

  return call(NODE_SERVICE, &Client::nodeUnstageVolume, std::move(request))
    .then(process::defer(self(), [this, volumeId, volumeState] {
      CHECK(volumes.contains(volumeId));

      if (volumeState.pre_provisioned()) {
        // Since this volume was pre-provisioned, it has reached the end of its
        // lifecycle. Remove it now.
        removeVolume(volumeId);
      } else {
        VolumeState& volumeState = volumes.at(volumeId).state;
        volumeState.set_state(VolumeState::NODE_READY);
        volumeState.clear_boot_id();

        checkpointVolumeState(volumeId);
      }

      return Nothing();
    }));
}


Future<Nothing> VolumeManagerProcess::__unpublishVolume(const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::VOL_READY) {
    return Nothing();
  }

  if (volumeState.state() != VolumeState::PUBLISHED &&
      volumeState.state() != VolumeState::NODE_PUBLISH &&
      volumeState.state() != VolumeState::NODE_UNPUBLISH) {
    return Failure(
        "Cannot unpublish volume '" + volumeId + "' in " +
        stringify(volumeState.state()) + "state");
  }

  // A previously failed `NodePublishVolume` call can be recovered through the
  // current `NodeUnpublishVolume` call. See:
  // https://github.com/container-storage-interface/spec/blob/v0.2.0/spec.md#nodepublishvolume // NOLINT
  if (volumeState.state() == VolumeState::PUBLISHED ||
      volumeState.state() == VolumeState::NODE_PUBLISH) {
    volumeState.set_state(VolumeState::NODE_UNPUBLISH);
    checkpointVolumeState(volumeId);
  }

  const string targetPath = paths::getMountTargetPath(mountRootDir, volumeId);

  CHECK(os::exists(targetPath));

  LOG(INFO) << "Calling '/csi.v0.Node/NodeUnpublishVolume' for volume '"
            << volumeId << "'";

  NodeUnpublishVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_target_path(targetPath);

  return call(NODE_SERVICE, &Client::nodeUnpublishVolume, std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::VOL_READY);

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


void VolumeManagerProcess::checkpointVolumeState(const string& volumeId)
{
  const string statePath =
    paths::getVolumeStatePath(rootDir, info.type(), info.name(), volumeId);

  // NOTE: We ensure the checkpoint is synced to the filesystem to avoid
  // resulting in a stale or empty checkpoint when a system crash happens.
  Try<Nothing> checkpoint =
    slave::state::checkpoint(statePath, volumes.at(volumeId).state, true);

  CHECK_SOME(checkpoint)
    << "Failed to checkpoint volume state to '" << statePath << "':"
    << checkpoint.error();
}


void VolumeManagerProcess::garbageCollectMountPath(const string& volumeId)
{
  CHECK(!volumes.contains(volumeId));

  const string path = paths::getMountPath(mountRootDir, volumeId);
  if (os::exists(path)) {
    Try<Nothing> rmdir = os::rmdir(path);
    if (rmdir.isError()) {
      LOG(ERROR) << "Failed to remove directory '" << path
                 << "': " << rmdir.error();
    }
  }
}


void VolumeManagerProcess::removeVolume(const string& volumeId)
{
  volumes.erase(volumeId);

  const string volumePath =
    paths::getVolumePath(rootDir, info.type(), info.name(), volumeId);

  Try<Nothing> rmdir = os::rmdir(volumePath);
  CHECK_SOME(rmdir) << "Failed to remove checkpointed volume state at '"
                    << volumePath << "': " << rmdir.error();

  garbageCollectMountPath(volumeId);
}


Future<Map<string, string>> VolumeManagerProcess::resolveSecrets(
    const Map<string, Secret>& secrets)
{
  if (!secretResolver) {
    return Failure(
        "CSI volume included secrets but the agent was not initialized with "
        "a secret resolver");
  }

  // This `futures` is used below with `process::collect()` to synchronize the
  // continuation. Within the continuation itself, we need to have the
  // key:value mapping of the secrets, so we use `resolvedSecrets` instead.
  vector<Future<Secret::Value>> futures;
  hashmap<string, Future<Secret::Value>> resolvedSecrets;

  for (auto it = secrets.begin(); it != secrets.end(); ++it) {
    Future<Secret::Value> pendingSecret = secretResolver->resolve(it->second);

    futures.push_back(pendingSecret);
    resolvedSecrets.insert({it->first, pendingSecret});
  }

  return process::collect(futures)
    .then([=]() {
      Map<string, string> result;

      foreachpair (
          const string& key,
          const Future<Secret::Value>& secret,
          resolvedSecrets) {
        CHECK(secret.isReady());

        result.insert({key, secret->data()});
      }

      return result;
    });
}


VolumeManager::VolumeManager(
    const string& rootDir,
    const CSIPluginInfo& info,
    const hashset<Service>& services,
    const Runtime& runtime,
    ServiceManager* serviceManager,
    Metrics* metrics,
    SecretResolver* secretResolver)
  : process(new VolumeManagerProcess(
        rootDir,
        info,
        services,
        runtime,
        serviceManager,
        metrics,
        secretResolver))
{
  process::spawn(CHECK_NOTNULL(process.get()));
  recovered = process::dispatch(process.get(), &VolumeManagerProcess::recover);
}


VolumeManager::~VolumeManager()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> VolumeManager::recover()
{
  return recovered;
}


Future<vector<VolumeInfo>> VolumeManager::listVolumes()
{
  return recovered
    .then(process::defer(process.get(), &VolumeManagerProcess::listVolumes));
}


Future<Bytes> VolumeManager::getCapacity(
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return recovered
    .then(process::defer(
        process.get(),
        &VolumeManagerProcess::getCapacity,
        capability,
        parameters));
}


Future<VolumeInfo> VolumeManager::createVolume(
    const string& name,
    const Bytes& capacity,
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return recovered
    .then(process::defer(
        process.get(),
        &VolumeManagerProcess::createVolume,
        name,
        capacity,
        capability,
        parameters));
}


Future<Option<Error>> VolumeManager::validateVolume(
    const VolumeInfo& volumeInfo,
    const CSIVolume::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return recovered
    .then(process::defer(
        process.get(),
        &VolumeManagerProcess::validateVolume,
        volumeInfo,
        capability,
        parameters));
}


Future<bool> VolumeManager::deleteVolume(const string& volumeId)
{
  return recovered
    .then(process::defer(
        process.get(), &VolumeManagerProcess::deleteVolume, volumeId));
}


Future<Nothing> VolumeManager::attachVolume(const string& volumeId)
{
  return recovered
    .then(process::defer(
        process.get(), &VolumeManagerProcess::attachVolume, volumeId));
}


Future<Nothing> VolumeManager::detachVolume(const string& volumeId)
{
  return recovered
    .then(process::defer(
        process.get(), &VolumeManagerProcess::detachVolume, volumeId));
}


Future<Nothing> VolumeManager::publishVolume(
    const string& volumeId,
    const Option<VolumeState>& volumeState)
{
  return recovered
    .then(process::defer(
        process.get(),
        &VolumeManagerProcess::publishVolume,
        volumeId,
        volumeState));
}


Future<Nothing> VolumeManager::unpublishVolume(const string& volumeId)
{
  return recovered
    .then(process::defer(
        process.get(), &VolumeManagerProcess::unpublishVolume, volumeId));
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
