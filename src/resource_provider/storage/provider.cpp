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
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <numeric>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/push_gauge.hpp>

#include <mesos/http.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
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

#include "csi/client.hpp"
#include "csi/metrics.hpp"
#include "csi/paths.hpp"
#include "csi/rpc.hpp"
#include "csi/service_manager.hpp"
#include "csi/state.hpp"
#include "csi/utils.hpp"
#include "csi/volume_manager.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/detector.hpp"
#include "resource_provider/state.hpp"

#include "resource_provider/storage/provider_process.hpp"

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "status_update_manager/operation.hpp"

namespace http = process::http;

// TODO(chhsiao): Remove `using namespace` statements after refactoring.
using namespace mesos::csi;
using namespace mesos::csi::v0;

using std::accumulate;
using std::find;
using std::list;
using std::queue;
using std::shared_ptr;
using std::string;
using std::vector;

using google::protobuf::Map;

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
using process::ProcessBase;
using process::Promise;
using process::Sequence;
using process::spawn;

using process::grpc::StatusError;

using process::http::authentication::Principal;

using process::metrics::Counter;
using process::metrics::PushGauge;

using mesos::csi::ServiceManager;

using mesos::csi::state::VolumeState;

using mesos::internal::protobuf::convertLabelsToStringMap;
using mesos::internal::protobuf::convertStringMapToLabels;

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


// Returns the parent endpoint as a URL.
// TODO(jieyu): Consider using a more reliable way to get the agent v1
// operator API endpoint URL.
static inline http::URL extractParentEndpoint(const http::URL& url)
{
  http::URL parent = url;

  parent.path = Path(url.path).dirname();

  return parent;
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
    resourceVersion(id::UUID::random()),
    sequence("storage-local-resource-provider-sequence"),
    metrics("resource_providers/" + info.type() + "." + info.name() + "/")
{
  diskProfileAdaptor = DiskProfileAdaptor::getAdaptor();
  CHECK_NOTNULL(diskProfileAdaptor.get());
}


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


template <
    csi::v0::RPC rpc,
    typename std::enable_if<rpc != csi::v0::PROBE, int>::type>
Future<csi::v0::Response<rpc>> StorageLocalResourceProviderProcess::call(
    const csi::Service& service,
    const csi::v0::Request<rpc>& request,
    const bool retry) // Make immutable in the following mutable lambda.
{
  Duration maxBackoff = DEFAULT_CSI_RETRY_BACKOFF_FACTOR;

  return loop(
      self(),
      [=] {
        // Make the call to the latest service endpoint.
        return serviceManager->getServiceEndpoint(service)
          .then(defer(
              self(),
              &StorageLocalResourceProviderProcess::_call<rpc>,
              lambda::_1,
              request));
      },
      [=](const Try<csi::v0::Response<rpc>, StatusError>& result) mutable
          -> Future<ControlFlow<csi::v0::Response<rpc>>> {
        Option<Duration> backoff = retry
          ? maxBackoff * (static_cast<double>(os::random()) / RAND_MAX)
          : Option<Duration>::none();

        maxBackoff = std::min(maxBackoff * 2, DEFAULT_CSI_RETRY_INTERVAL_MAX);

        // We dispatch `__call` for testing purpose.
        return dispatch(
            self(),
            &StorageLocalResourceProviderProcess::__call<rpc>,
            result,
            backoff);
      });
}


template <csi::v0::RPC rpc>
Future<Try<csi::v0::Response<rpc>, StatusError>>
StorageLocalResourceProviderProcess::_call(
    const string& endpoint, const csi::v0::Request<rpc>& request)
{
  ++metrics.csi_plugin_rpcs_pending.at(rpc);

  return csi::v0::Client(endpoint, runtime).call<rpc>(request)
    .onAny(defer(self(), [=](
        const Future<Try<csi::v0::Response<rpc>, StatusError>>& future) {
      --metrics.csi_plugin_rpcs_pending.at(rpc);
      if (future.isReady() && future->isSome()) {
        ++metrics.csi_plugin_rpcs_successes.at(rpc);
      } else if (future.isDiscarded()) {
        ++metrics.csi_plugin_rpcs_cancelled.at(rpc);
      } else {
        ++metrics.csi_plugin_rpcs_errors.at(rpc);
      }
    }));
}


template <csi::v0::RPC rpc>
Future<ControlFlow<csi::v0::Response<rpc>>>
StorageLocalResourceProviderProcess::__call(
    const Try<csi::v0::Response<rpc>, StatusError>& result,
    const Option<Duration>& backoff)
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
      LOG(ERROR)
        << "Received '" << result.error() << "' while calling " << rpc
        << ". Retrying in " << backoff.get();

      return after(backoff.get())
        .then([]() -> Future<ControlFlow<csi::v0::Response<rpc>>> {
          return Continue();
        });
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


void StorageLocalResourceProviderProcess::initialize()
{
  const Principal principal = LocalResourceProvider::principal(info);
  CHECK(principal.claims.contains("cid_prefix"));
  const string& containerPrefix = principal.claims.at("cid_prefix");

  rootDir = slave::paths::getCsiRootDir(workDir);
  pluginInfo = info.storage().plugin();
  services = {CONTROLLER_SERVICE, NODE_SERVICE};

  serviceManager.reset(new ServiceManager(
      extractParentEndpoint(url),
      rootDir,
      pluginInfo,
      services,
      containerPrefix,
      authToken,
      runtime,
      &metrics));

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

  return recoverVolumes()
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

      LOG(INFO) << "Finished recovery for resource provider with type '"
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


Future<Nothing> StorageLocalResourceProviderProcess::recoverVolumes()
{
  Try<string> bootId_ = os::bootId();
  if (bootId_.isError()) {
    return Failure("Failed to get boot ID: " + bootId_.error());
  }

  bootId = bootId_.get();

  return serviceManager->recover()
    .then(process::defer(self(), &Self::prepareServices))
    .then(process::defer(self(), [this]() -> Future<Nothing> {
      // Recover the states of CSI volumes.
      Try<list<string>> volumePaths =
        paths::getVolumePaths(rootDir, pluginInfo.type(), pluginInfo.name());

      if (volumePaths.isError()) {
        return Failure(
            "Failed to find volumes for CSI plugin type '" + pluginInfo.type() +
            "' and name '" + pluginInfo.name() + "': " + volumePaths.error());
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

        CHECK_EQ(pluginInfo.type(), volumePath->type);
        CHECK_EQ(pluginInfo.name(), volumePath->name);

        const string& volumeId = volumePath->volumeId;
        const string statePath = paths::getVolumeStatePath(
            rootDir, pluginInfo.type(), pluginInfo.name(), volumeId);

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
      const string mountRootDir =
        paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name());

      Try<list<string>> mountPaths = paths::getMountPaths(mountRootDir);
      if (mountPaths.isError()) {
        // TODO(chhsiao): This could indicate that something is seriously wrong.
        // To help debugging the problem, we should surface the error via
        // MESOS-8745.
        return Failure(
            "Failed to find mount paths for CSI plugin type '" +
            pluginInfo.type() + "' and name '" + pluginInfo.name() +
            "': " + mountPaths.error());
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
      futures.push_back(publishVolume(volumeId));
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


Future<Nothing> StorageLocalResourceProviderProcess::prepareServices()
{
  CHECK(!services.empty());

  // Get the plugin capabilities.
  return call<GET_PLUGIN_CAPABILITIES>(
      *services.begin(), GetPluginCapabilitiesRequest())
    .then(process::defer(self(), [=](
        const GetPluginCapabilitiesResponse& response) -> Future<Nothing> {
      pluginCapabilities = response.capabilities();

      if (services.contains(CONTROLLER_SERVICE) &&
          !pluginCapabilities->controllerService) {
        return Failure(
            "CONTROLLER_SERVICE plugin capability is not supported for CSI "
            "plugin type '" +
            pluginInfo.type() + "' and name '" + pluginInfo.name() + "'");
      }

      return Nothing();
    }))
    // Check if all services have consistent plugin infos.
    .then(process::defer(self(), [this] {
      vector<Future<GetPluginInfoResponse>> futures;
      foreach (const Service& service, services) {
        futures.push_back(
            call<GET_PLUGIN_INFO>(CONTROLLER_SERVICE, GetPluginInfoRequest())
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

      return call<CONTROLLER_GET_CAPABILITIES>(
          CONTROLLER_SERVICE, ControllerGetCapabilitiesRequest())
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

      return call<NODE_GET_CAPABILITIES>(
          NODE_SERVICE, NodeGetCapabilitiesRequest())
        .then(process::defer(self(), [this](
            const NodeGetCapabilitiesResponse& response) -> Future<Nothing> {
          nodeCapabilities = response.capabilities();

          if (controllerCapabilities->publishUnpublishVolume) {
            return call<NODE_GET_ID>(NODE_SERVICE, NodeGetIdRequest())
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


Future<Nothing> StorageLocalResourceProviderProcess::publishVolume(
    const string& volumeId)
{
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


Future<Nothing> StorageLocalResourceProviderProcess::_attachVolume(
    const string& volumeId)
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
    csi::v0::evolve(volumeState.volume_capability());
  request.set_readonly(false);
  *request.mutable_volume_attributes() = volumeState.volume_attributes();

  return call<CONTROLLER_PUBLISH_VOLUME>(CONTROLLER_SERVICE, std::move(request))
    .then(process::defer(self(), [this, volumeId](
        const ControllerPublishVolumeResponse& response) {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::NODE_READY);
      *volumeState.mutable_publish_info() = response.publish_info();

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::_detachVolume(
    const string& volumeId)
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

  return call<CONTROLLER_UNPUBLISH_VOLUME>(
      CONTROLLER_SERVICE, std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::CREATED);
      volumeState.mutable_publish_info()->clear();

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::_publishVolume(
    const string& volumeId)
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

  const string targetPath = paths::getMountTargetPath(
      paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
      volumeId);

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
  *request.mutable_publish_info() = volumeState.publish_info();
  request.set_target_path(targetPath);
  *request.mutable_volume_capability() =
    csi::v0::evolve(volumeState.volume_capability());
  request.set_readonly(false);
  *request.mutable_volume_attributes() = volumeState.volume_attributes();

  if (nodeCapabilities->stageUnstageVolume) {
    const string stagingPath = paths::getMountStagingPath(
        paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
        volumeId);

    CHECK(os::exists(stagingPath));
    request.set_staging_target_path(stagingPath);
  }

  return call<NODE_PUBLISH_VOLUME>(NODE_SERVICE, std::move(request))
    .then(defer(self(), [this, volumeId, targetPath] {
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


Future<Nothing> StorageLocalResourceProviderProcess::__publishVolume(
    const string& volumeId)
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

  const string stagingPath = paths::getMountStagingPath(
      paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
      volumeId);

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
  *request.mutable_publish_info() = volumeState.publish_info();
  request.set_staging_target_path(stagingPath);
  *request.mutable_volume_capability() =
    csi::v0::evolve(volumeState.volume_capability());
  *request.mutable_volume_attributes() = volumeState.volume_attributes();

  return call<NODE_STAGE_VOLUME>(NODE_SERVICE, std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::VOL_READY);
      volumeState.set_boot_id(CHECK_NOTNONE(bootId));

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::_unpublishVolume(
    const string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.state() == VolumeState::NODE_READY) {
    CHECK(volumeState.boot_id().empty());
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
    // Since this is a no-op, no need to checkpoint here.
    volumeState.set_state(VolumeState::NODE_READY);
    volumeState.clear_boot_id();
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

  const string stagingPath = paths::getMountStagingPath(
      paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
      volumeId);

  CHECK(os::exists(stagingPath));

  LOG(INFO) << "Calling '/csi.v0.Node/NodeUnstageVolume' for volume '"
            << volumeId << "'";

  NodeUnstageVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_staging_target_path(stagingPath);

  return call<NODE_UNSTAGE_VOLUME>(NODE_SERVICE, std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::NODE_READY);
      volumeState.clear_boot_id();

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<Nothing> StorageLocalResourceProviderProcess::__unpublishVolume(
    const string& volumeId)
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

  const string targetPath = paths::getMountTargetPath(
      paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
      volumeId);

  CHECK(os::exists(targetPath));

  LOG(INFO) << "Calling '/csi.v0.Node/NodeUnpublishVolume' for volume '"
            << volumeId << "'";

  NodeUnpublishVolumeRequest request;
  request.set_volume_id(volumeId);
  request.set_target_path(targetPath);

  return call<NODE_UNPUBLISH_VOLUME>(NODE_SERVICE, std::move(request))
    .then(process::defer(self(), [this, volumeId] {
      CHECK(volumes.contains(volumeId));
      VolumeState& volumeState = volumes.at(volumeId).state;
      volumeState.set_state(VolumeState::VOL_READY);

      checkpointVolumeState(volumeId);

      return Nothing();
    }));
}


Future<VolumeInfo> StorageLocalResourceProviderProcess::createVolume(
    const string& name,
    const Bytes& capacity,
    const types::VolumeCapability& capability,
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
  *request.add_volume_capabilities() = csi::v0::evolve(capability);
  *request.mutable_parameters() = parameters;

  // We retry the `CreateVolume` call for MESOS-9517.
  return call<CREATE_VOLUME>(CONTROLLER_SERVICE, std::move(request), true)
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
      *volumeState.mutable_volume_attributes() = response.volume().attributes();

      volumes.put(volumeId, std::move(volumeState));
      checkpointVolumeState(volumeId);

      return VolumeInfo{capacity, volumeId, response.volume().attributes()};
    }));
}


Future<bool> StorageLocalResourceProviderProcess::deleteVolume(
    const string& volumeId)
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


Future<bool> StorageLocalResourceProviderProcess::_deleteVolume(
    const std::string& volumeId)
{
  CHECK(volumes.contains(volumeId));
  VolumeState& volumeState = volumes.at(volumeId).state;

  if (volumeState.node_publish_required()) {
    CHECK_EQ(VolumeState::PUBLISHED, volumeState.state());

    const string targetPath = paths::getMountTargetPath(
        paths::getMountRootDir(rootDir, pluginInfo.type(), pluginInfo.name()),
        volumeId);

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
      volumes.erase(volumeId);

      const string volumePath = paths::getVolumePath(
          rootDir, pluginInfo.type(), pluginInfo.name(), volumeId);

      Try<Nothing> rmdir = os::rmdir(volumePath);
      CHECK_SOME(rmdir) << "Failed to remove checkpointed volume state at '"
                        << volumePath << "': " << rmdir.error();

      garbageCollectMountPath(volumeId);

      return deleted;
    }));
}


Future<bool> StorageLocalResourceProviderProcess::__deleteVolume(
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
  return call<DELETE_VOLUME>(CONTROLLER_SERVICE, std::move(request), true)
    .then([] { return true; });
}


Future<Option<Error>> StorageLocalResourceProviderProcess::validateVolume(
    const VolumeInfo& volumeInfo,
    const types::VolumeCapability& capability,
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
  *request.add_volume_capabilities() = csi::v0::evolve(capability);
  *request.mutable_volume_attributes() = volumeInfo.context;

  return call<VALIDATE_VOLUME_CAPABILITIES>(
      CONTROLLER_SERVICE, std::move(request))
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
      *volumeState.mutable_volume_attributes() = volumeInfo.context;

      volumes.put(volumeInfo.id, std::move(volumeState));
      checkpointVolumeState(volumeInfo.id);

      return None();
    }));
}


Future<Resources> StorageLocalResourceProviderProcess::listVolumes()
{
  CHECK(info.has_id());

  // This is only used for reconciliation so no failure is returned.
  if (!controllerCapabilities->listVolumes) {
    return Resources();
  }

  // TODO(chhsiao): Set the max entries and use a loop to do
  // multiple `ListVolumes` calls.
  return call<csi::v0::LIST_VOLUMES>(
      csi::CONTROLLER_SERVICE, csi::v0::ListVolumesRequest())
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
            vendor,
            entry.volume().id(),
            entry.volume().attributes().empty()
              ? Option<Labels>::none()
              : convertStringMapToLabels(entry.volume().attributes()));
      }

      return resources;
    }));
}


Future<Resources> StorageLocalResourceProviderProcess::getCapacities()
{
  CHECK(info.has_id());

  // This is only used for reconciliation so no failure is returned.
  if (!controllerCapabilities->getCapacity) {
    return Resources();
  }

  vector<Future<Resources>> futures;

  foreachpair (const string& profile,
               const DiskProfileAdaptor::ProfileInfo& profileInfo,
               profileInfos) {
    csi::v0::GetCapacityRequest request;
    *request.add_volume_capabilities() =
      csi::v0::evolve(profileInfo.capability);
    *request.mutable_parameters() = profileInfo.parameters;

    futures.push_back(
        call<csi::v0::GET_CAPACITY>(
            csi::CONTROLLER_SERVICE, std::move(request))
          .then(defer(self(), [=](
              const csi::v0::GetCapacityResponse& response) -> Resources {
            if (response.available_capacity() == 0) {
              return Resources();
            }

            return createRawDiskResource(
                info, Bytes(response.available_capacity()), profile, vendor);
          })));
  }

  return collect(futures)
    .then([](const vector<Resources>& resources) {
      return accumulate(resources.begin(), resources.end(), Resources());
    });
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
  // afterward. See MESOS-9254.
  Future<VolumeInfo> created;
  if (resource.disk().source().has_profile()) {
    created = createVolume(
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

    created = validateVolume(
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

  return deleteVolume(resource.disk().source().id())
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
            reconciled = sequence.add(std::function<Future<Nothing>()>(
                defer(self(), &Self::reconcileStoragePools)));
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

    // TODO(chhsiao): Support persistent BLOCK volumes.
    if (resource.disk().source().type() != Resource::DiskInfo::Source::MOUNT) {
      return Error(
          "Cannot create persistent volume '" +
          stringify(resource.disk().persistence().id()) + "' on a " +
          stringify(resource.disk().source().type()) + " disk");
    }
  }

  return getResourceConversions(operation);
}


Try<vector<ResourceConversion>>
StorageLocalResourceProviderProcess::applyDestroy(
    const Offer::Operation& operation) const
{
  CHECK(operation.has_destroy());

  foreach (const Resource& resource, operation.destroy().volumes()) {
    // TODO(chhsiao): Support cleaning up persistent BLOCK volumes, presumably
    // with `dd` or any other utility to zero out the block device.
    CHECK(Resources::isPersistentVolume(resource));
    CHECK(resource.disk().source().type() == Resource::DiskInfo::Source::MOUNT);
    CHECK(resource.disk().source().has_id());

    const string& volumeId = resource.disk().source().id();
    CHECK(volumes.contains(volumeId));

    const VolumeState& volumeState = volumes.at(volumeId).state;

    // NOTE: Data can only be written to the persistent volume when when it is
    // in `PUBLISHED` state (i.e., mounted). Once a volume has been transitioned
    // to `PUBLISHED`, we will set the `node_publish_required` field and always
    // recover it back to `PUBLISHED` after a failover, until a `DESTROY_DISK`
    // is applied, which only comes after `DESTROY`. So we only need to clean up
    // the volume if it has the field set.
    if (!volumeState.node_publish_required()) {
      continue;
    }

    CHECK_EQ(VolumeState::PUBLISHED, volumeState.state());

    const string targetPath = csi::paths::getMountTargetPath(
        csi::paths::getMountRootDir(
            slave::paths::getCsiRootDir(workDir),
            info.storage().plugin().type(),
            info.storage().plugin().name()),
        volumeId);

    // Only the data in the target path, but not itself, should be removed.
    Try<Nothing> rmdir = os::rmdir(targetPath, true, false);
    if (rmdir.isError()) {
      return Error(
          "Failed to remove persistent volume '" +
          stringify(resource.disk().persistence().id()) + "' at '" +
          targetPath + "': " + rmdir.error());
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


void StorageLocalResourceProviderProcess::garbageCollectMountPath(
    const string& volumeId)
{
  CHECK(!volumes.contains(volumeId));

  const string path = csi::paths::getMountPath(
      csi::paths::getMountRootDir(
          slave::paths::getCsiRootDir(workDir),
          info.storage().plugin().type(),
          info.storage().plugin().name()),
      volumeId);

  if (os::exists(path)) {
    Try<Nothing> rmdir = os::rmdir(path);
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
