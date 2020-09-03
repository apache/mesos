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

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/map.h>
#include <google/protobuf/message.h>

#include <grpcpp/grpcpp.h>

#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>

#include <mesos/type_utils.hpp>

#include <mesos/csi/v0.hpp>
#include <mesos/csi/v1.hpp>

#include <process/grpc.hpp>
#include <process/http.hpp>

#include <stout/adaptor.hpp>
#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/some.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/rmdir.hpp>

#include "csi/v0_utils.hpp"
#include "csi/v1_utils.hpp"
#include "csi/volume_manager.hpp"

#include "linux/fs.hpp"

#include "logging/logging.hpp"

namespace http = process::http;
namespace internal = mesos::internal;

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::max;
using std::min;
using std::string;
using std::unique_ptr;
using std::vector;

using google::protobuf::Map;
using google::protobuf::MapPair;
using google::protobuf::RepeatedPtrField;

using grpc::AsyncGenericService;
using grpc::ByteBuffer;
using grpc::ClientContext;
using grpc::GenericClientAsyncResponseReader;
using grpc::GenericServerAsyncReaderWriter;
using grpc::GenericServerContext;
using grpc::GenericStub;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using grpc::WriteOptions;

using process::grpc::StatusError;

using VolumeCapability = mesos::Volume::Source::CSIVolume::VolumeCapability;


constexpr char PLUGIN_NAME[] = "org.apache.mesos.csi.test";
constexpr char NODE_ID[] = "localhost";
constexpr Bytes DEFAULT_VOLUME_CAPACITY = Megabytes(64);


class Flags : public virtual mesos::internal::logging::Flags
{
public:
  Flags()
  {
    const hashset<string> supportedApiVersions =
      {mesos::csi::v0::API_VERSION, mesos::csi::v1::API_VERSION};

    add(&Flags::api_version,
        "api_version",
        "If set, the plugin would only serve CSI calls of the specified API\n"
        "version. Otherwise, the plugin serves all supported API versions by\n"
        "default. (Supported values: " + stringify(supportedApiVersions) + ")",
        [=](const Option<string>& apiVersion) {
          return apiVersion.isNone() ||
            supportedApiVersions.contains(apiVersion.get())
              ? Option<Error>::none() : Error("Unsupported API version");
        });

    add(&Flags::endpoint,
        "endpoint",
        "Path to the Unix domain socket the plugin should bind to.");

    add(&Flags::work_dir,
        "work_dir",
        "Path to the work directory of the plugin.");

    add(&Flags::available_capacity,
        "available_capacity",
        "The available disk capacity managed by the plugin, in addition\n"
        "to the pre-existing volumes specified in the --volumes flag.");

    add(&Flags::create_parameters,
        "create_parameters",
        "The parameters required for volume creation. The parameters are\n"
        "specified as a semicolon-delimited list of param=value pairs.\n"
        "(Example: 'param1=value1;param2=value2')");

    add(&Flags::volume_metadata,
        "volume_metadata",
        "The static properties to add to the contextual information of each\n"
        "volume. The metadata are specified as a semicolon-delimited list of\n"
        "prop=value pairs. (Example: 'prop1=value1;prop2=value2')");

    add(&Flags::volumes,
        "volumes",
        "Creates preprovisioned volumes upon start-up. The volumes are\n"
        "specified as a semicolon-delimited list of name:capacity pairs.\n"
        "If a volume with the same name already exists, the pair will be\n"
        "ignored. (Example: 'volume1:1GB;volume2:2GB')");

    add(&Flags::volume_id_path,
        "volume_id_path",
        "When set to true, this flag causes the volume ID of all volumes to\n"
        "be set to the volume's path.",
        true);

    add(&Flags::forward,
        "forward",
        "If set, the plugin forwards all requests to the specified Unix\n"
        "domain socket. (Example: 'unix:///path/to/socket')");
  }

  Option<string> api_version;
  string endpoint;
  string work_dir;
  Bytes available_capacity;
  Option<string> create_parameters;
  Option<string> volume_metadata;
  Option<string> volumes;
  bool volume_id_path;
  Option<string> forward;
};


struct VolumeInfo
{
  Bytes capacity;
  string id;
  string path;
  google::protobuf::Map<std::string, std::string> context;
};


class TestCSIPlugin
  : public csi::v0::Identity::Service,
    public csi::v0::Controller::Service,
    public csi::v0::Node::Service,
    public csi::v1::Identity::Service,
    public csi::v1::Controller::Service,
    public csi::v1::Node::Service
{
public:
  TestCSIPlugin(
      const Option<string>& _apiVersion,
      const string& _endpoint,
      const string& _workDir,
      const Bytes& _availableCapacity,
      const hashmap<string, string>& _createParameters,
      const hashmap<string, string>& _volumeMetadata,
      const hashmap<string, Bytes>& _volumes,
      bool _volumeIdPath)
    : apiVersion(_apiVersion),
      endpoint(_endpoint),
      workDir(_workDir),
      availableCapacity(_availableCapacity),
      createParameters(_createParameters.begin(), _createParameters.end()),
      volumeMetadata(_volumeMetadata.begin(), _volumeMetadata.end()),
      volumeIdPath(_volumeIdPath)
  {
    // Construct the default mount volume capability.
    defaultVolumeCapability.mutable_mount();
    defaultVolumeCapability.mutable_access_mode()
      ->set_mode(VolumeCapability::AccessMode::SINGLE_NODE_WRITER);

    Bytes usedCapacity(0);

    // Scan for created volumes.
    //
    // TODO(jieyu): Consider not using CHECKs here.
    Try<list<string>> paths = fs::list(path::join(workDir, "*-*"));
    foreach (const string& path, CHECK_NOTERROR(paths)) {
      Try<VolumeInfo> createdVolume = CHECK_NOTERROR(parseVolumePath(path));
      volumes.put(createdVolume->id, createdVolume.get());
      usedCapacity += createdVolume->capacity;
    }

    // Create preprovisioned volumes if they have not existed yet.
    foreachpair (const string& name, const Bytes& capacity, _volumes) {
      Option<VolumeInfo> found = findVolumeByName(name);

      if (found.isSome()) {
        CHECK_EQ(found->capacity, capacity)
          << "Expected preprovisioned volume '" << name << "' to be "
          << capacity << " but found " << found->capacity << " instead";

        usedCapacity -= found->capacity;
        continue;
      }


      VolumeInfo volumeInfo =
        createVolumeInfo(capacity, name, volumeMetadata);

      Try<Nothing> mkdir = os::mkdir(volumeInfo.path);
      CHECK_SOME(mkdir)
        << "Failed to create directory for preprovisioned volume '" << name
        << "': " << mkdir.error();

      volumes.put(volumeInfo.id, std::move(volumeInfo));
    }

    CHECK_GE(availableCapacity, usedCapacity)
      << "Insufficient available capacity for volumes, expected to be at least "
      << usedCapacity;

    availableCapacity -= usedCapacity;
  }

  void run();

  // CSI v0 RPCs.

  Status GetPluginInfo(
      ServerContext* context,
      const csi::v0::GetPluginInfoRequest* request,
      csi::v0::GetPluginInfoResponse* response) override;

  Status GetPluginCapabilities(
      ServerContext* context,
      const csi::v0::GetPluginCapabilitiesRequest* request,
      csi::v0::GetPluginCapabilitiesResponse* response) override;

  Status Probe(
      ServerContext* context,
      const csi::v0::ProbeRequest* request,
      csi::v0::ProbeResponse* response) override;

  Status CreateVolume(
      ServerContext* context,
      const csi::v0::CreateVolumeRequest* request,
      csi::v0::CreateVolumeResponse* response) override;

  Status DeleteVolume(
      ServerContext* context,
      const csi::v0::DeleteVolumeRequest* request,
      csi::v0::DeleteVolumeResponse* response) override;

  Status ControllerPublishVolume(
      ServerContext* context,
      const csi::v0::ControllerPublishVolumeRequest* request,
      csi::v0::ControllerPublishVolumeResponse* response) override;

  Status ControllerUnpublishVolume(
      ServerContext* context,
      const csi::v0::ControllerUnpublishVolumeRequest* request,
      csi::v0::ControllerUnpublishVolumeResponse* response) override;

  Status ValidateVolumeCapabilities(
      ServerContext* context,
      const csi::v0::ValidateVolumeCapabilitiesRequest* request,
      csi::v0::ValidateVolumeCapabilitiesResponse* response) override;

  Status ListVolumes(
      ServerContext* context,
      const csi::v0::ListVolumesRequest* request,
      csi::v0::ListVolumesResponse* response) override;

  Status GetCapacity(
      ServerContext* context,
      const csi::v0::GetCapacityRequest* request,
      csi::v0::GetCapacityResponse* response) override;

  Status ControllerGetCapabilities(
      ServerContext* context,
      const csi::v0::ControllerGetCapabilitiesRequest* request,
      csi::v0::ControllerGetCapabilitiesResponse* response) override;

  Status NodeStageVolume(
      ServerContext* context,
      const csi::v0::NodeStageVolumeRequest* request,
      csi::v0::NodeStageVolumeResponse* response) override;

  Status NodeUnstageVolume(
      ServerContext* context,
      const csi::v0::NodeUnstageVolumeRequest* request,
      csi::v0::NodeUnstageVolumeResponse* response) override;

  Status NodePublishVolume(
      ServerContext* context,
      const csi::v0::NodePublishVolumeRequest* request,
      csi::v0::NodePublishVolumeResponse* response) override;

  Status NodeUnpublishVolume(
      ServerContext* context,
      const csi::v0::NodeUnpublishVolumeRequest* request,
      csi::v0::NodeUnpublishVolumeResponse* response) override;

  Status NodeGetId(
      ServerContext* context,
      const csi::v0::NodeGetIdRequest* request,
      csi::v0::NodeGetIdResponse* response) override;

  Status NodeGetCapabilities(
      ServerContext* context,
      const csi::v0::NodeGetCapabilitiesRequest* request,
      csi::v0::NodeGetCapabilitiesResponse* response) override;

  // CSI v1 RPCs.

  Status GetPluginInfo(
      ServerContext* context,
      const csi::v1::GetPluginInfoRequest* request,
      csi::v1::GetPluginInfoResponse* response) override;

  Status GetPluginCapabilities(
      ServerContext* context,
      const csi::v1::GetPluginCapabilitiesRequest* request,
      csi::v1::GetPluginCapabilitiesResponse* response) override;

  Status Probe(
      ServerContext* context,
      const csi::v1::ProbeRequest* request,
      csi::v1::ProbeResponse* response) override;

  Status CreateVolume(
      ServerContext* context,
      const csi::v1::CreateVolumeRequest* request,
      csi::v1::CreateVolumeResponse* response) override;

  Status DeleteVolume(
      ServerContext* context,
      const csi::v1::DeleteVolumeRequest* request,
      csi::v1::DeleteVolumeResponse* response) override;

  Status ControllerPublishVolume(
      ServerContext* context,
      const csi::v1::ControllerPublishVolumeRequest* request,
      csi::v1::ControllerPublishVolumeResponse* response) override;

  Status ControllerUnpublishVolume(
      ServerContext* context,
      const csi::v1::ControllerUnpublishVolumeRequest* request,
      csi::v1::ControllerUnpublishVolumeResponse* response) override;

  Status ValidateVolumeCapabilities(
      ServerContext* context,
      const csi::v1::ValidateVolumeCapabilitiesRequest* request,
      csi::v1::ValidateVolumeCapabilitiesResponse* response) override;

  Status ListVolumes(
      ServerContext* context,
      const csi::v1::ListVolumesRequest* request,
      csi::v1::ListVolumesResponse* response) override;

  Status GetCapacity(
      ServerContext* context,
      const csi::v1::GetCapacityRequest* request,
      csi::v1::GetCapacityResponse* response) override;

  Status ControllerGetCapabilities(
      ServerContext* context,
      const csi::v1::ControllerGetCapabilitiesRequest* request,
      csi::v1::ControllerGetCapabilitiesResponse* response) override;

  Status NodeStageVolume(
      ServerContext* context,
      const csi::v1::NodeStageVolumeRequest* request,
      csi::v1::NodeStageVolumeResponse* response) override;

  Status NodeUnstageVolume(
      ServerContext* context,
      const csi::v1::NodeUnstageVolumeRequest* request,
      csi::v1::NodeUnstageVolumeResponse* response) override;

  Status NodePublishVolume(
      ServerContext* context,
      const csi::v1::NodePublishVolumeRequest* request,
      csi::v1::NodePublishVolumeResponse* response) override;

  Status NodeUnpublishVolume(
      ServerContext* context,
      const csi::v1::NodeUnpublishVolumeRequest* request,
      csi::v1::NodeUnpublishVolumeResponse* response) override;

  Status NodeGetCapabilities(
      ServerContext* context,
      const csi::v1::NodeGetCapabilitiesRequest* request,
      csi::v1::NodeGetCapabilitiesResponse* response) override;

  Status NodeGetInfo(
      ServerContext* context,
      const csi::v1::NodeGetInfoRequest* request,
      csi::v1::NodeGetInfoResponse* response) override;

private:
  string getVolumePath(const Bytes& capacity, const string& name);
  Try<VolumeInfo> parseVolumePath(const string& dir);
  Option<VolumeInfo> findVolumeByName(const string& name);

  // Creates a volume info with the specified name based on the
  // value of the `volume_id_path` flag.
  VolumeInfo createVolumeInfo(
      const Bytes& _capacity,
      const string& name,
      const google::protobuf::Map<string, string> context);


  Try<VolumeInfo, StatusError> createVolume(
      const string& name,
      const Bytes& requiredBytes,
      const Bytes& limitBytes,
      const RepeatedPtrField<VolumeCapability>& capabilities,
      const Map<string, string> parameters);

  Try<Nothing, StatusError> deleteVolume(const string& volumeId);

  Try<Nothing, StatusError> controllerPublishVolume(
      const string& volumeId,
      const string& nodeId,
      const VolumeCapability& capability,
      bool readonly,
      const Map<string, string>& volumeContext);

  Try<Nothing, StatusError> controllerUnpublishVolume(
      const string& volumeId, const string& nodeId);

  // Returns `StatusError` if the volume does not exist; returns `Option<Error>`
  // with an error set if the volume is not compatible with the given arguments.
  Try<Option<Error>, StatusError> validateVolumeCapabilities(
      const string& volumeId,
      const Map<string, string>& volumeContext,
      const RepeatedPtrField<VolumeCapability>& capabilities,
      const Option<Map<string, string>>& parameters = None());

  Try<vector<VolumeInfo>, StatusError> listVolumes(
      const Option<int32_t>& maxEntries,
      const Option<string>& startingToken);

  Try<Bytes, StatusError> getCapacity(
      const RepeatedPtrField<VolumeCapability>& capabilities,
      const Map<string, string>& parameters);

  Try<Nothing, StatusError> nodeStageVolume(
      const string& volumeId,
      const Map<string, string>& publishContext,
      const string& stagingPath,
      const VolumeCapability& capability,
      const Map<string, string>& volumeContext);

  Try<Nothing, StatusError> nodeUnstageVolume(
      const string& volumeId, const string& stagingPath);

  Try<Nothing, StatusError> nodePublishVolume(
      const string& volumeId,
      const Map<string, string>& publishContext,
      const string& stagingPath,
      const string& targetPath,
      const VolumeCapability& capability,
      bool readonly,
      const Map<string, string>& volumeContext);

  Try<Nothing, StatusError> nodeUnpublishVolume(
      const string& volumeId, const string& targetPath);

  const Option<string> apiVersion;
  const string endpoint;
  const string workDir;

  Bytes availableCapacity;
  VolumeCapability defaultVolumeCapability;
  Map<string, string> createParameters;
  Map<string, string> volumeMetadata;
  hashmap<string, VolumeInfo> volumes;
  bool volumeIdPath;
};


void TestCSIPlugin::run()
{
  ServerBuilder builder;
  builder.AddListeningPort(endpoint, grpc::InsecureServerCredentials());

  if (apiVersion.isNone() || apiVersion.get() == mesos::csi::v0::API_VERSION) {
    builder.RegisterService(static_cast<csi::v0::Identity::Service*>(this));
    builder.RegisterService(static_cast<csi::v0::Controller::Service*>(this));
    builder.RegisterService(static_cast<csi::v0::Node::Service*>(this));
  }

  if (apiVersion.isNone() || apiVersion.get() == mesos::csi::v1::API_VERSION) {
    builder.RegisterService(static_cast<csi::v1::Identity::Service*>(this));
    builder.RegisterService(static_cast<csi::v1::Controller::Service*>(this));
    builder.RegisterService(static_cast<csi::v1::Node::Service*>(this));
  }

  std::unique_ptr<Server> server = builder.BuildAndStart();
  server->Wait();
}


Status TestCSIPlugin::GetPluginInfo(
    ServerContext* context,
    const csi::v0::GetPluginInfoRequest* request,
    csi::v0::GetPluginInfoResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->set_name(PLUGIN_NAME);
  response->set_vendor_version(MESOS_VERSION);

  return Status::OK;
}


Status TestCSIPlugin::GetPluginCapabilities(
    ServerContext* context,
    const csi::v0::GetPluginCapabilitiesRequest* request,
    csi::v0::GetPluginCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_capabilities()->mutable_service()->set_type(
      csi::v0::PluginCapability::Service::CONTROLLER_SERVICE);

  return Status::OK;
}


Status TestCSIPlugin::Probe(
    ServerContext* context,
    const csi::v0::ProbeRequest* request,
    csi::v0::ProbeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  return Status::OK;
}


Status TestCSIPlugin::CreateVolume(
    ServerContext* context,
    const csi::v0::CreateVolumeRequest* request,
    csi::v0::CreateVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<VolumeInfo, StatusError> result = createVolume(
      request->name(),
      request->capacity_range().required_bytes()
        ? request->capacity_range().required_bytes() : 1,
      request->capacity_range().limit_bytes()
        ? request->capacity_range().limit_bytes()
        : std::numeric_limits<int64_t>::max(),
      mesos::csi::v0::devolve(request->volume_capabilities()),
      request->parameters());

  if (result.isError()) {
    return result.error().status;
  }

  response->mutable_volume()->set_id(result->id);
  response->mutable_volume()->set_capacity_bytes(result->capacity.bytes());
  *response->mutable_volume()->mutable_attributes() = result->context;

  return Status::OK;
}


Status TestCSIPlugin::DeleteVolume(
    ServerContext* context,
    const csi::v0::DeleteVolumeRequest* request,
    csi::v0::DeleteVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = deleteVolume(request->volume_id());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ControllerPublishVolume(
    ServerContext* context,
    const csi::v0::ControllerPublishVolumeRequest* request,
    csi::v0::ControllerPublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = controllerPublishVolume(
      request->volume_id(),
      request->node_id(),
      mesos::csi::v0::devolve(request->volume_capability()),
      request->readonly(),
      request->volume_attributes());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ControllerUnpublishVolume(
    ServerContext* context,
    const csi::v0::ControllerUnpublishVolumeRequest* request,
    csi::v0::ControllerUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    controllerUnpublishVolume(request->volume_id(), request->node_id());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ValidateVolumeCapabilities(
    ServerContext* context,
    const csi::v0::ValidateVolumeCapabilitiesRequest* request,
    csi::v0::ValidateVolumeCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Option<Error>, StatusError> result = validateVolumeCapabilities(
      request->volume_id(),
      request->volume_attributes(),
      mesos::csi::v0::devolve(request->volume_capabilities()));

  if (result.isError()) {
    return result.error().status;
  }

  if (result->isSome()) {
    response->set_supported(false);
    response->set_message(result->get().message);
  } else {
    response->set_supported(true);
  }

  return Status::OK;
}


Status TestCSIPlugin::ListVolumes(
    ServerContext* context,
    const csi::v0::ListVolumesRequest* request,
    csi::v0::ListVolumesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Try<vector<VolumeInfo>, StatusError> result = listVolumes(
      request->max_entries()
        ? request->max_entries() : Option<int32_t>::none(),
      !request->starting_token().empty()
        ? request->starting_token() : Option<string>::none());

  if (result.isError()) {
    return result.error().status;
  }

  foreach (const VolumeInfo& volumeInfo, result.get()) {
    csi::v0::Volume* volume = response->add_entries()->mutable_volume();
    volume->set_id(volumeInfo.id);
    volume->set_capacity_bytes(volumeInfo.capacity.bytes());
    *volume->mutable_attributes() = volumeInfo.context;
  }

  return Status::OK;
}


Status TestCSIPlugin::GetCapacity(
    ServerContext* context,
    const csi::v0::GetCapacityRequest* request,
    csi::v0::GetCapacityResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Try<Bytes, StatusError> result = getCapacity(
      mesos::csi::v0::devolve(request->volume_capabilities()),
      request->parameters());

  if (result.isError()) {
    return result.error().status;
  }

  response->set_available_capacity(result->bytes());

  return Status::OK;
}


Status TestCSIPlugin::ControllerGetCapabilities(
    ServerContext* context,
    const csi::v0::ControllerGetCapabilitiesRequest* request,
    csi::v0::ControllerGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  const vector<csi::v0::ControllerServiceCapability::RPC::Type> rpcs = {
    csi::v0::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME,
    csi::v0::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME,
    csi::v0::ControllerServiceCapability::RPC::GET_CAPACITY,
    csi::v0::ControllerServiceCapability::RPC::LIST_VOLUMES,
  };

  foreach (const csi::v0::ControllerServiceCapability::RPC::Type rpc, rpcs) {
    response->add_capabilities()->mutable_rpc()->set_type(rpc);
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeStageVolume(
    ServerContext* context,
    const csi::v0::NodeStageVolumeRequest* request,
    csi::v0::NodeStageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = nodeStageVolume(
      request->volume_id(),
      request->publish_info(),
      request->staging_target_path(),
      mesos::csi::v0::devolve(request->volume_capability()),
      request->volume_attributes());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnstageVolume(
    ServerContext* context,
    const csi::v0::NodeUnstageVolumeRequest* request,
    csi::v0::NodeUnstageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    nodeUnstageVolume(request->volume_id(), request->staging_target_path());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodePublishVolume(
    ServerContext* context,
    const csi::v0::NodePublishVolumeRequest* request,
    csi::v0::NodePublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = nodePublishVolume(
      request->volume_id(),
      request->publish_info(),
      request->staging_target_path(),
      request->target_path(),
      mesos::csi::v0::devolve(request->volume_capability()),
      request->readonly(),
      request->volume_attributes());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnpublishVolume(
    ServerContext* context,
    const csi::v0::NodeUnpublishVolumeRequest* request,
    csi::v0::NodeUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    nodeUnpublishVolume(request->volume_id(), request->target_path());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeGetId(
    ServerContext* context,
    const csi::v0::NodeGetIdRequest* request,
    csi::v0::NodeGetIdResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->set_node_id(NODE_ID);

  return Status::OK;
}


Status TestCSIPlugin::NodeGetCapabilities(
    ServerContext* context,
    const csi::v0::NodeGetCapabilitiesRequest* request,
    csi::v0::NodeGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v0::NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME);

  return Status::OK;
}


Status TestCSIPlugin::GetPluginInfo(
    ServerContext* context,
    const csi::v1::GetPluginInfoRequest* request,
    csi::v1::GetPluginInfoResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // Since CSI v1 requires the plugin name to follow the (forward) domain name
  // notation, we reverse the plugin's reverse-DNS name here.
  response->set_name(
      strings::join(".", adaptor::reverse(strings::split(".", PLUGIN_NAME))));

  response->set_vendor_version(MESOS_VERSION);

  return Status::OK;
}


Status TestCSIPlugin::GetPluginCapabilities(
    ServerContext* context,
    const csi::v1::GetPluginCapabilitiesRequest* request,
    csi::v1::GetPluginCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_capabilities()->mutable_service()->set_type(
      csi::v1::PluginCapability::Service::CONTROLLER_SERVICE);

  return Status::OK;
}


Status TestCSIPlugin::Probe(
    ServerContext* context,
    const csi::v1::ProbeRequest* request,
    csi::v1::ProbeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->mutable_ready()->set_value(true);

  return Status::OK;
}


Status TestCSIPlugin::CreateVolume(
    ServerContext* context,
    const csi::v1::CreateVolumeRequest* request,
    csi::v1::CreateVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<VolumeInfo, StatusError> result = createVolume(
      request->name(),
      request->capacity_range().required_bytes()
        ? request->capacity_range().required_bytes() : 1,
      request->capacity_range().limit_bytes()
        ? request->capacity_range().limit_bytes()
        : std::numeric_limits<int64_t>::max(),
      mesos::csi::v1::devolve(request->volume_capabilities()),
      request->parameters());

  if (result.isError()) {
    return result.error().status;
  }

  response->mutable_volume()->set_volume_id(result->id);
  response->mutable_volume()->set_capacity_bytes(result->capacity.bytes());
  *response->mutable_volume()->mutable_volume_context() = result->context;

  return Status::OK;
}


Status TestCSIPlugin::DeleteVolume(
    ServerContext* context,
    const csi::v1::DeleteVolumeRequest* request,
    csi::v1::DeleteVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = deleteVolume(request->volume_id());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ControllerPublishVolume(
    ServerContext* context,
    const csi::v1::ControllerPublishVolumeRequest* request,
    csi::v1::ControllerPublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = controllerPublishVolume(
      request->volume_id(),
      request->node_id(),
      mesos::csi::v1::devolve(request->volume_capability()),
      request->readonly(),
      request->volume_context());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ControllerUnpublishVolume(
    ServerContext* context,
    const csi::v1::ControllerUnpublishVolumeRequest* request,
    csi::v1::ControllerUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    controllerUnpublishVolume(request->volume_id(), request->node_id());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::ValidateVolumeCapabilities(
    ServerContext* context,
    const csi::v1::ValidateVolumeCapabilitiesRequest* request,
    csi::v1::ValidateVolumeCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Option<Error>, StatusError> result = validateVolumeCapabilities(
      request->volume_id(),
      request->volume_context(),
      mesos::csi::v1::devolve(request->volume_capabilities()),
      request->parameters());

  if (result.isError()) {
    return result.error().status;
  }

  if (result->isSome()) {
    response->set_message(result->get().message);
  } else {
    *response->mutable_confirmed()->mutable_volume_context() =
      request->volume_context();
    *response->mutable_confirmed()->mutable_volume_capabilities() =
      request->volume_capabilities();
    *response->mutable_confirmed()->mutable_parameters() =
      request->parameters();
  }

  return Status::OK;
}


Status TestCSIPlugin::ListVolumes(
    ServerContext* context,
    const csi::v1::ListVolumesRequest* request,
    csi::v1::ListVolumesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Try<vector<VolumeInfo>, StatusError> result = listVolumes(
      request->max_entries()
        ? request->max_entries() : Option<int32_t>::none(),
      !request->starting_token().empty()
        ? request->starting_token() : Option<string>::none());

  if (result.isError()) {
    return result.error().status;
  }

  foreach (const VolumeInfo& volumeInfo, result.get()) {
    csi::v1::Volume* volume = response->add_entries()->mutable_volume();
    volume->set_volume_id(volumeInfo.id);
    volume->set_capacity_bytes(volumeInfo.capacity.bytes());
    *volume->mutable_volume_context() = volumeInfo.context;
  }

  return Status::OK;
}


Status TestCSIPlugin::GetCapacity(
    ServerContext* context,
    const csi::v1::GetCapacityRequest* request,
    csi::v1::GetCapacityResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Try<Bytes, StatusError> result = getCapacity(
      mesos::csi::v1::devolve(request->volume_capabilities()),
      request->parameters());

  if (result.isError()) {
    return result.error().status;
  }

  response->set_available_capacity(result->bytes());

  return Status::OK;
}


string TestCSIPlugin::getVolumePath(const Bytes& capacity, const string& name)
{
  return path::join(workDir, strings::join("-", capacity, http::encode(name)));
}


Status TestCSIPlugin::ControllerGetCapabilities(
    ServerContext* context,
    const csi::v1::ControllerGetCapabilitiesRequest* request,
    csi::v1::ControllerGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  const vector<csi::v1::ControllerServiceCapability::RPC::Type> rpcs = {
    csi::v1::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME,
    csi::v1::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME,
    csi::v1::ControllerServiceCapability::RPC::GET_CAPACITY,
    csi::v1::ControllerServiceCapability::RPC::LIST_VOLUMES,
  };

  foreach (const csi::v1::ControllerServiceCapability::RPC::Type rpc, rpcs) {
    response->add_capabilities()->mutable_rpc()->set_type(rpc);
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeStageVolume(
    ServerContext* context,
    const csi::v1::NodeStageVolumeRequest* request,
    csi::v1::NodeStageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result = nodeStageVolume(
      request->volume_id(),
      request->publish_context(),
      request->staging_target_path(),
      mesos::csi::v1::devolve(request->volume_capability()),
      request->volume_context());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnstageVolume(
    ServerContext* context,
    const csi::v1::NodeUnstageVolumeRequest* request,
    csi::v1::NodeUnstageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    nodeUnstageVolume(request->volume_id(), request->staging_target_path());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodePublishVolume(
    ServerContext* context,
    const csi::v1::NodePublishVolumeRequest* request,
    csi::v1::NodePublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  // Since CSI v1 states that creation of the target path is the responsibility
  // of the plugin, we create the mount point here if it does not exist.
  if (!os::exists(request->target_path())) {
    Try<Nothing> mkdir = os::mkdir(request->target_path());
    if (mkdir.isError()) {
      return Status(
          grpc::INTERNAL,
          "Failed to create target path '" + request->target_path() +
            "' for volume '" + request->volume_id() + "': " + mkdir.error());
    }
  }

  Try<Nothing, StatusError> result = nodePublishVolume(
      request->volume_id(),
      request->publish_context(),
      request->staging_target_path(),
      request->target_path(),
      mesos::csi::v1::devolve(request->volume_capability()),
      request->readonly(),
      request->volume_context());

  if (result.isError()) {
    return result.error().status;
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnpublishVolume(
    ServerContext* context,
    const csi::v1::NodeUnpublishVolumeRequest* request,
    csi::v1::NodeUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate the request.

  Try<Nothing, StatusError> result =
    nodeUnpublishVolume(request->volume_id(), request->target_path());

  if (result.isError()) {
    return result.error().status;
  }

  // Since CSI v1 requires the plugin to delete the target path, we delete it
  // here if it has not been deleted by a previous attempt.
  if (os::exists(request->target_path())) {
    Try<Nothing> rmdir = os::rmdir(request->target_path());
    if (rmdir.isError()) {
      return Status(
          grpc::INTERNAL,
          "Failed to remove target path '" + request->target_path() +
            "' for volume '" + request->volume_id() + "': " + rmdir.error());
    }
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeGetCapabilities(
    ServerContext* context,
    const csi::v1::NodeGetCapabilitiesRequest* request,
    csi::v1::NodeGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v1::NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME);

  return Status::OK;
}


Status TestCSIPlugin::NodeGetInfo(
    ServerContext* context,
    const csi::v1::NodeGetInfoRequest* request,
    csi::v1::NodeGetInfoResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->set_node_id(NODE_ID);

  return Status::OK;
}


Try<VolumeInfo> TestCSIPlugin::parseVolumePath(const string& dir)
{
  // TODO(chhsiao): Consider using `<regex>`, which requires GCC 4.9+.

  // Make sure there's a separator at the end of the prefix so that we
  // don't accidentally slice off part of a directory.
  const string prefix = path::join(workDir, "");

  if (!strings::startsWith(dir, prefix)) {
    return Error(
        "Directory '" + dir + "' does not fall under work directory '" +
        prefix + "'");
  }

  const string basename = Path(dir).basename();

  vector<string> tokens = strings::tokenize(basename, "-", 2);
  if (tokens.size() != 2) {
    return Error("Cannot find delimiter '-' in '" + basename + "'");
  }

  Try<Bytes> capacity = Bytes::parse(tokens[0]);
  if (capacity.isError()) {
    return Error(
        "Failed to parse capacity from '" + tokens[0] +
        "': " + capacity.error());
  }

  Try<string> name = http::decode(tokens[1]);
  if (name.isError()) {
    return Error(
        "Failed to decode volume name from '" + tokens[1] +
        "': " + name.error());
  }

  CHECK_EQ(dir, getVolumePath(capacity.get(), name.get()))
    << "Cannot reconstruct volume path '" << dir << "' from volume name '"
    << name.get() << "' and capacity " << capacity.get();

  const string volumeId = volumeIdPath ? dir : name.get();

  return VolumeInfo{capacity.get(), volumeId, dir, volumeMetadata};
}


Option<VolumeInfo> TestCSIPlugin::findVolumeByName(const string& name)
{
  foreachvalue (const VolumeInfo& volumeInfo, volumes) {
    const string volumeId =
      volumeIdPath ? getVolumePath(volumeInfo.capacity, name) : name;

    if (volumeInfo.id == volumeId) {
      return volumeInfo;
    }
  }

  return None();
}


VolumeInfo TestCSIPlugin::createVolumeInfo(
    const Bytes& capacity,
    const string& name,
    const google::protobuf::Map<string, string> context)
{
  const string volumeId = volumeIdPath ? getVolumePath(capacity, name) : name;

  return VolumeInfo{
      capacity,
      volumeId,
      getVolumePath(capacity, name),
      context};
}


Try<VolumeInfo, StatusError> TestCSIPlugin::createVolume(
    const string& name,
    const Bytes& requiredBytes,
    const Bytes& limitBytes,
    const RepeatedPtrField<VolumeCapability>& capabilities,
    const Map<string, string> parameters)
{
  // The volume ID is determined by `name`, with reserved characters escaped.
  const string volumeId = http::encode(name);

  foreach (const VolumeCapability& capability, capabilities) {
    if (capability != defaultVolumeCapability) {
      return StatusError(Status(
          grpc::INVALID_ARGUMENT, "Unsupported volume capabilities"));
    }
  }

  if (parameters != createParameters) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Unsupported create parameters"));
  }

  Option<VolumeInfo> found = findVolumeByName(name);

  if (found.isSome()) {
    if (found->capacity > limitBytes) {
      return StatusError(Status(
          grpc::ALREADY_EXISTS, "Cannot satisfy limit bytes"));
    }

    if (found->capacity < requiredBytes) {
      return StatusError(Status(
          grpc::ALREADY_EXISTS, "Cannot satisfy required bytes"));
    }

    return *found;
  } else {
    if (availableCapacity < requiredBytes) {
      return StatusError(Status(grpc::OUT_OF_RANGE, "Insufficient capacity"));
    }


    // We assume that `requiredBytes <= limitBytes` has been verified.
    const Bytes defaultSize = min(availableCapacity, DEFAULT_VOLUME_CAPACITY);

    VolumeInfo volumeInfo = createVolumeInfo(
        min(max(defaultSize, requiredBytes), limitBytes),
        name,
        volumeMetadata);

    Try<Nothing> mkdir = os::mkdir(volumeInfo.path);
    if (mkdir.isError()) {
      return StatusError(Status(
          grpc::INTERNAL,
          "Failed to create volume '" + volumeInfo.id + "': " + mkdir.error()));
    }

    CHECK_GE(availableCapacity, volumeInfo.capacity);
    availableCapacity -= volumeInfo.capacity;
    volumes.put(volumeInfo.id, volumeInfo);

    return volumeInfo;
  }

  UNREACHABLE();
}


Try<Nothing, StatusError> TestCSIPlugin::deleteVolume(const string& volumeId)
{
  if (!volumes.contains(volumeId)) {
    // Return a success for idempotency.
    return Nothing();
  }

  const VolumeInfo& volumeInfo = volumes.at(volumeId);

  Try<Nothing> rmdir = os::rmdir(volumeInfo.path);
  if (rmdir.isError()) {
    return StatusError(Status(
        grpc::INTERNAL,
        "Failed to delete volume '" + volumeId + "': " + rmdir.error()));
  }

  availableCapacity += volumeInfo.capacity;
  volumes.erase(volumeInfo.id);

  return Nothing();
}


Try<Nothing, StatusError> TestCSIPlugin::controllerPublishVolume(
    const string& volumeId,
    const string& nodeId,
    const VolumeCapability& capability,
    bool readonly,
    const Map<string, string>& volumeContext)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  if (nodeId != NODE_ID) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Node '" + nodeId + "' does not exist"));
  }

  if (capability != defaultVolumeCapability) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Unsupported volume capability"));
  }

  if (readonly) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Unsupported read-only mode"));
  }

  const VolumeInfo& volumeInfo = volumes.at(volumeId);

  if (volumeContext != volumeInfo.context) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid volume context"));
  }

  // Do nothing.
  return Nothing();
}


Try<Nothing, StatusError> TestCSIPlugin::controllerUnpublishVolume(
    const string& volumeId, const string& nodeId)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  if (nodeId != NODE_ID) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Node '" + nodeId + "' does not exist"));
  }

  // Do nothing.
  return Nothing();
}


Try<Option<Error>, StatusError> TestCSIPlugin::validateVolumeCapabilities(
    const string& volumeId,
    const Map<string, string>& volumeContext,
    const RepeatedPtrField<VolumeCapability>& capabilities,
    const Option<Map<string, string>>& parameters)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  const VolumeInfo& volumeInfo = volumes.at(volumeId);

  if (volumeContext != volumeInfo.context) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid volume context"));
  }

  foreach (const VolumeCapability& capability, capabilities) {
    if (capability != defaultVolumeCapability) {
      return Some(Error("Unsupported volume capabilities"));
    }
  }

  if (parameters.isSome() && parameters.get() != createParameters) {
    return Some(Error("Mismatched parameters"));
  }

  return None();
}


Try<vector<VolumeInfo>, StatusError> TestCSIPlugin::listVolumes(
    const Option<int32_t>& maxEntries, const Option<string>& startingToken)
{
  // TODO(chhsiao): Support max entries.
  if (maxEntries.isSome()) {
    return StatusError(Status(
        grpc::ABORTED, "Specifying max entries is not supported"));
  }

  // TODO(chhsiao): Support starting token.
  if (startingToken.isSome()) {
    return StatusError(Status(
        grpc::ABORTED, "Specifying starting token is not supported"));
  }

  return volumes.values();
}


Try<Bytes, StatusError> TestCSIPlugin::getCapacity(
    const RepeatedPtrField<VolumeCapability>& capabilities,
    const Map<string, string>& parameters)
{
  // We report zero capacity if any capability other than the default mount
  // volume capability is given. If no capacity is given, the total available
  // capacity  will be returned.
  foreach (const VolumeCapability& capability, capabilities) {
    if (capability != defaultVolumeCapability) {
      return Bytes(0);
    }
  }

  if (parameters != createParameters) {
    return Bytes(0);
  }

  return availableCapacity;
}


Try<Nothing, StatusError> TestCSIPlugin::nodeStageVolume(
    const string& volumeId,
    const Map<string, string>& publishContext,
    const string& stagingPath,
    const VolumeCapability& capability,
    const Map<string, string>& volumeContext)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  if (!publishContext.empty()) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid publish context"));
  }

  if (!os::exists(stagingPath)) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT,
        "Staging path '" + stagingPath + "' does not exist"));
  }

  if (capability != defaultVolumeCapability) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Unsupported volume capability"));
  }

  const VolumeInfo& volumeInfo = volumes.at(volumeId);

  if (volumeContext != volumeInfo.context) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid volume context"));
  }

  Try<internal::fs::MountInfoTable> table =
    internal::fs::MountInfoTable::read();

  if (table.isError()) {
    return StatusError(Status(
        grpc::INTERNAL, "Failed to get mount table: " + table.error()));
  }

  if (std::any_of(
          table->entries.begin(),
          table->entries.end(),
          [&](const internal::fs::MountInfoTable::Entry& entry) {
            return entry.target == stagingPath;
          })) {
    return Nothing();
  }

  Try<Nothing> mount = internal::fs::mount(
      volumeInfo.path,
      stagingPath,
      None(),
      MS_BIND,
      None());

  if (mount.isError()) {
    return StatusError(Status(
        grpc::INTERNAL,
        "Failed to mount from '" + volumeInfo.path + "' to '" + stagingPath +
          "': " + mount.error()));
  }

  return Nothing();
}


Try<Nothing, StatusError> TestCSIPlugin::nodeUnstageVolume(
    const string& volumeId, const string& stagingPath)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  Try<internal::fs::MountInfoTable> table =
    internal::fs::MountInfoTable::read();

  if (table.isError()) {
    return StatusError(Status(
        grpc::INTERNAL, "Failed to get mount table: " + table.error()));
  }

  if (std::none_of(
          table->entries.begin(),
          table->entries.end(),
          [&](const internal::fs::MountInfoTable::Entry& entry) {
            return entry.target == stagingPath;
          })) {
    return Nothing();
  }

  Try<Nothing> unmount = internal::fs::unmount(stagingPath);
  if (unmount.isError()) {
    return StatusError(Status(
        grpc::INTERNAL,
        "Failed to unmount '" + stagingPath + "': " + unmount.error()));
  }

  return Nothing();
}


Try<Nothing, StatusError> TestCSIPlugin::nodePublishVolume(
    const string& volumeId,
    const Map<string, string>& publishContext,
    const string& stagingPath,
    const string& targetPath,
    const VolumeCapability& capability,
    bool readonly,
    const Map<string, string>& volumeContext)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  if (!publishContext.empty()) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid publish context"));
  }

  if (!os::exists(targetPath)) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT,
        "Target path '" + targetPath + "' does not exist"));
  }

  if (capability != defaultVolumeCapability) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Unsupported volume capability"));
  }

  const VolumeInfo& volumeInfo = volumes.at(volumeId);

  if (volumeContext != volumeInfo.context) {
    return StatusError(Status(
        grpc::INVALID_ARGUMENT, "Invalid volume context"));
  }

  Try<internal::fs::MountInfoTable> table =
    internal::fs::MountInfoTable::read();

  if (table.isError()) {
    return StatusError(Status(
        grpc::INTERNAL, "Failed to get mount table: " + table.error()));
  }

  if (std::none_of(
          table->entries.begin(),
          table->entries.end(),
          [&](const internal::fs::MountInfoTable::Entry& entry) {
            return entry.target == stagingPath;
          })) {
    return StatusError(Status(
        grpc::FAILED_PRECONDITION,
        "Volume '" + volumeId + "' has not been staged yet"));
  }

  if (std::any_of(
          table->entries.begin(),
          table->entries.end(),
          [&](const internal::fs::MountInfoTable::Entry& entry) {
            return entry.target == targetPath;
          })) {
    return Nothing();
  }

  Try<Nothing> mount = internal::fs::mount(
      stagingPath,
      targetPath,
      None(),
      MS_BIND | (readonly ? MS_RDONLY : 0),
      None());

  if (mount.isError()) {
    return StatusError(Status(
        grpc::INTERNAL,
        "Failed to mount from '" + stagingPath + "' to '" + targetPath +
          "': " + mount.error()));
  }

  return Nothing();
}


Try<Nothing, StatusError> TestCSIPlugin::nodeUnpublishVolume(
    const string& volumeId, const string& targetPath)
{
  if (!volumes.contains(volumeId)) {
    return StatusError(Status(
        grpc::NOT_FOUND, "Volume '" + volumeId + "' does not exist"));
  }

  Try<internal::fs::MountInfoTable> table =
    internal::fs::MountInfoTable::read();

  if (table.isError()) {
    return StatusError(Status(
        grpc::INTERNAL, "Failed to get mount table: " + table.error()));
  }

  if (std::none_of(
          table->entries.begin(),
          table->entries.end(),
          [&](const internal::fs::MountInfoTable::Entry& entry) {
            return entry.target == targetPath;
          })) {
    return Nothing();
  }

  Try<Nothing> unmount = internal::fs::unmount(targetPath);
  if (unmount.isError()) {
    return StatusError(Status(
        grpc::INTERNAL,
        "Failed to unmount '" + targetPath + "': " + unmount.error()));
  }

  return Nothing();
}


// Serves CSI calls from the given endpoint through forwarding the calls to
// another CSI endpoint and returning back the results.
class CSIProxy
{
public:
  CSIProxy(
      const Option<string>& _apiVersion,
      const string& _endpoint,
      const string& forward)
    : apiVersion(_apiVersion),
      endpoint(_endpoint),
      stub(grpc::CreateChannel(forward, grpc::InsecureChannelCredentials())),
      service(new AsyncGenericService()) {}

  void run();

private:
  struct Call
  {
    Call() : state(State::INITIALIZED), serverReaderWriter(&serverContext) {}

    enum class State {
      INITIALIZED,
      REQUESTED,
      FORWARDING,
      FINISHING,
    } state;

    GenericServerContext serverContext;
    GenericServerAsyncReaderWriter serverReaderWriter;
    ClientContext clientContext;
    std::unique_ptr<GenericClientAsyncResponseReader> clientReader;

    ByteBuffer request;
    ByteBuffer response;
    Status status;
  };

  void serve(ServerCompletionQueue* completionQueue);

  const Option<string> apiVersion;
  const string endpoint;

  GenericStub stub;
  std::unique_ptr<AsyncGenericService> service;
};


void CSIProxy::run()
{
  ServerBuilder builder;
  builder.AddListeningPort(endpoint, grpc::InsecureServerCredentials());
  builder.RegisterAsyncGenericService(service.get());

  std::unique_ptr<ServerCompletionQueue> completionQueue =
    builder.AddCompletionQueue();

  std::unique_ptr<Server> server = builder.BuildAndStart();
  std::thread looper(std::bind(&CSIProxy::serve, this, completionQueue.get()));

  server->Wait();

  // Shutdown the completion queue after the server has been shut down. The
  // looper thread will then drain the queue before finishing. See:
  // https://grpc.io/grpc/cpp/classgrpc_1_1_server_builder.html#a960d55977e1aef56fd7b582037a01bbd // NOLINT
  completionQueue->Shutdown();
  looper.join();
}


// The lifecycle of a forwarded CSI call is shown as follows. The transitions
// happen after the completions of the API calls.
//
//                        Unsupported                  Server-side
//        +-------------+ API version +-------------+ WriteAndFinish +---+
//        | INITIALIZED |   +--------->  FINISHING  +----------------> X |
//        +------+------+   |         +------^------+                +---+
//   Server-side |   +------+                | Client-side
//   RequestCall |   |    Server-side        | Finish (unary call)
//        +------v---+--+    Read     +------+------+
//        |  REQUESTED  +-------------> FORWARDING  |
//        +-------------+             +-------------+
//
void CSIProxy::serve(ServerCompletionQueue* completionQueue)
{
  // Serve the first call. The ownership of the `Call` object is passed to the
  // completion queue, and will be retrieved later in the loop below.
  Call* first = new Call();
  service->RequestCall(
      &first->serverContext,
      &first->serverReaderWriter,
      completionQueue,
      completionQueue,
      first);

  void* tag;
  bool ok = false;

  // See the link below for details of `ok`:
  // https://grpc.io/grpc/cpp/classgrpc_1_1_completion_queue.html#a86d9810ced694e50f7987ac90b9f8c1a // NOLINT
  while (completionQueue->Next(&tag, &ok)) {
    // Retrieve the ownership of the next ready `Call` object from the
    // completion queue.
    Call* call = reinterpret_cast<Call*>(tag);

    switch (call->state) {
      case Call::State::INITIALIZED: {
        if (!ok) {
          // Server-side `RequestCall`: the server has been shutdown so continue
          // to drain the queue.
          break;
        }

        call->state = Call::State::REQUESTED;

        // Make a server-side `Read` call and return the ownership back to the
        // completion queue.
        call->serverReaderWriter.Read(&call->request, call);

        // Serve the next call while processing this one. The ownership of the
        // new `Call` object is passed to the completion queue, and will be
        // retrieved in a later iteration.
        Call* next = new Call();
        service->RequestCall(
            &next->serverContext,
            &next->serverReaderWriter,
            completionQueue,
            completionQueue,
            next);

        break;
      }
      case Call::State::REQUESTED: {
        if (!ok) {
          // Server-side `Read`: the client has done a `WritesDone` already, so
          // clean up the call and move to the next iteration immediately.
          delete call;
          continue;
        }

        // The expected method names are of the following form:
        //   /csi.<api_version>.<service_name>/<rpc_name>
        // See: https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#requests // NOLINT
        if (apiVersion.isSome() &&
            !strings::startsWith(
                call->serverContext.method(),
                "/csi." + apiVersion.get() + ".")) {
          // The proxy does not support the API version of the call so respond
          // with `UNIMPLEMENTED`.
          call->state = Call::State::FINISHING;
          call->status = Status(grpc::UNIMPLEMENTED, "");
          call->serverReaderWriter.WriteAndFinish(
              call->response, WriteOptions(), call->status, call);

          break;
        }

        LOG(INFO) << "Forwarding " << call->serverContext.method() << " call";
        call->state = Call::State::FORWARDING;

        call->clientContext.set_wait_for_ready(true);
        call->clientContext.set_deadline(call->serverContext.deadline());

        foreachpair (const grpc::string_ref& key,
                     const grpc::string_ref& value,
                     call->serverContext.client_metadata()) {
          call->clientContext.AddMetadata(
              grpc::string(key.data(), key.size()),
              grpc::string(value.data(), value.size()));
        }

        call->clientReader = stub.PrepareUnaryCall(
            &call->clientContext,
            call->serverContext.method(),
            call->request,
            completionQueue);

        call->clientReader->StartCall();

        // Make a client-side `Finish` call for the unary RPC and return the
        // ownership back to the completion queue.
        call->clientReader->Finish(&call->response, &call->status, call);

        break;
      }
      case Call::State::FORWARDING: {
        // Client-side `Finish` for unary RPCs: `ok` should always be true.
        CHECK(ok);

        call->state = Call::State::FINISHING;

        // Make a server-side `WriteAndFinish` call and return the ownership
        // back to the completion queue.
        call->serverReaderWriter.WriteAndFinish(
            call->response, WriteOptions(), call->status, call);

        break;
      }
      case Call::State::FINISHING: {
        if (!ok) {
          // Server-side `WriteAndFinish`: the response hasn't gone to the wire.
          LOG(ERROR) << "Failed to forward response for "
                     << call->serverContext.method() << " call";
        }

        // The call is completed so clean it up and move to the next iteration
        // immediately.
        delete call;
        continue;
      }
    }
  }
}


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("CSI_", argc, argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], true, flags);

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  Try<Nothing> mkdir = os::mkdir(flags.work_dir);
  if (mkdir.isError()) {
    cerr << "Failed to create working directory '" << flags.work_dir
         << "': " << mkdir.error() << endl;
    return EXIT_FAILURE;
  }

  hashmap<string, string> createParameters;

  if (flags.create_parameters.isSome()) {
    foreachpair (const string& param,
                 const vector<string>& values,
                 strings::pairs(flags.create_parameters.get(), ";", "=")) {
      if (values.size() != 1) {
        cerr << "Parameter key '" << param << "' is not unique" << endl;
        return EXIT_FAILURE;
      }

      createParameters.put(param, values[0]);
    }
  }

  hashmap<string, string> volumeMetadata;

  if (flags.volume_metadata.isSome()) {
    foreachpair (const string& prop,
                 const vector<string>& values,
                 strings::pairs(flags.volume_metadata.get(), ";", "=")) {
      if (values.size() != 1) {
        cerr << "Metadata key '" << prop << "' is not unique" << endl;
        return EXIT_FAILURE;
      }

      volumeMetadata.put(prop, values[0]);
    }
  }

  hashmap<string, Bytes> volumes;

  if (flags.volumes.isSome()) {
    foreachpair (const string& name,
                 const vector<string>& capacities,
                 strings::pairs(flags.volumes.get(), ";", ":")) {
      Option<Error> error;

      if (capacities.size() != 1) {
        error = "Volume name must be unique";
      } else {
        Try<Bytes> capacity = Bytes::parse(capacities[0]);
        if (capacity.isError()) {
          error = capacity.error();
        } else {
          volumes.put(name, capacity.get());
        }
      }

      if (error.isSome()) {
        cerr << "Failed to parse the '--volumes' flag: " << error.get() << endl;
        return EXIT_FAILURE;
      }
    }
  }

  // Terminate the plugin if the endpoint socket file already exists to simulate
  // an `EADDRINUSE` bind error.
  const string endpointPath =
    strings::remove(flags.endpoint, "unix://", strings::PREFIX);

  if (os::exists(endpointPath)) {
    cerr << "Failed to create endpoint '" << endpointPath << "': already exists"
         << endl;

    return EXIT_FAILURE;
  }

  if (flags.forward.isSome()) {
    CSIProxy proxy(flags.api_version, flags.endpoint, flags.forward.get());

    proxy.run();
  } else {
    TestCSIPlugin plugin(
        flags.api_version,
        flags.endpoint,
        flags.work_dir,
        flags.available_capacity,
        createParameters,
        volumeMetadata,
        volumes,
        flags.volume_id_path);

    plugin.run();
  }
}
