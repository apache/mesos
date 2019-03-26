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

#include <memory>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>

#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>

#include <mesos/type_utils.hpp>

#include <mesos/csi/v0.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/rmdir.hpp>

#include "csi/v0_utils.hpp"

#include "linux/fs.hpp"

#include "logging/logging.hpp"

namespace fs = mesos::internal::fs;

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::max;
using std::min;
using std::string;
using std::unique_ptr;
using std::vector;

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

constexpr char PLUGIN_NAME[] = "org.apache.mesos.csi.test";
constexpr char NODE_ID[] = "localhost";
constexpr Bytes DEFAULT_VOLUME_CAPACITY = Megabytes(64);


class Flags : public virtual mesos::internal::logging::Flags
{
public:
  Flags()
  {
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

    add(&Flags::volumes,
        "volumes",
        "Creates preprovisioned volumes upon start-up. The volumes are\n"
        "specified as a semicolon-delimited list of name:capacity pairs.\n"
        "If a volume with the same name already exists, the pair will be\n"
        "ignored. (Example: 'volume1:1GB;volume2:2GB')");

    add(&Flags::forward,
        "forward",
        "If set, the plugin forwards all requests to the specified Unix\n"
        "domain socket. (Example: 'unix:///path/to/socket')");
  }

  string endpoint;
  string work_dir;
  Bytes available_capacity;
  Option<string> create_parameters;
  Option<string> volumes;
  Option<string> forward;
};


class TestCSIPlugin
  : public csi::v0::Identity::Service,
    public csi::v0::Controller::Service,
    public csi::v0::Node::Service
{
public:
  TestCSIPlugin(
      const string& _workDir,
      const string& _endpoint,
      const Bytes& _availableCapacity,
      const hashmap<string, string>& _createParameters,
      const hashmap<string, Bytes>& _volumes)
    : workDir(_workDir),
      endpoint(_endpoint),
      availableCapacity(_availableCapacity),
      createParameters(_createParameters.begin(), _createParameters.end())
  {
    // Construct the default mount volume capability.
    defaultVolumeCapability.mutable_mount();
    defaultVolumeCapability.mutable_access_mode()
      ->set_mode(csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER);

    // Scan for preprovisioned volumes.
    //
    // TODO(jieyu): Consider not using CHECKs here.
    Try<list<string>> paths = os::ls(workDir);
    CHECK_SOME(paths);

    foreach (const string& path, paths.get()) {
      Try<VolumeInfo> volumeInfo = parseVolumePath(path);
      CHECK_SOME(volumeInfo);

      CHECK(!volumes.contains(volumeInfo->id));
      volumes.put(volumeInfo->id, volumeInfo.get());

      if (!_volumes.contains(volumeInfo->id)) {
        CHECK_GE(availableCapacity, volumeInfo->size);
        availableCapacity -= volumeInfo->size;
      }
    }

    foreachpair (const string& name, const Bytes& capacity, _volumes) {
      if (volumes.contains(name)) {
        continue;
      }

      VolumeInfo volumeInfo;
      volumeInfo.id = name;
      volumeInfo.size = capacity;

      const string path = getVolumePath(volumeInfo);

      Try<Nothing> mkdir = os::mkdir(path);
      CHECK_SOME(mkdir);

      volumes.put(volumeInfo.id, volumeInfo);
    }
  }

  void run();

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

private:
  struct VolumeInfo
  {
    string id;
    Bytes size;
  };

  string getVolumePath(const VolumeInfo& volumeInfo);
  Try<VolumeInfo> parseVolumePath(const string& path);

  const string workDir;
  const string endpoint;

  Bytes availableCapacity;
  csi::v0::VolumeCapability defaultVolumeCapability;
  google::protobuf::Map<string, string> createParameters;
  hashmap<string, VolumeInfo> volumes;
};


void TestCSIPlugin::run()
{
  ServerBuilder builder;
  builder.AddListeningPort(endpoint, grpc::InsecureServerCredentials());
  builder.RegisterService(static_cast<csi::v0::Identity::Service*>(this));
  builder.RegisterService(static_cast<csi::v0::Controller::Service*>(this));
  builder.RegisterService(static_cast<csi::v0::Node::Service*>(this));

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

  // TODO(chhsiao): Validate required fields.

  if (request->name().empty()) {
    return Status(grpc::INVALID_ARGUMENT, "Volume name cannot be empty");
  }

  if (strings::contains(request->name(), stringify(os::PATH_SEPARATOR))) {
    return Status(
        grpc::INVALID_ARGUMENT,
        "Volume name cannot contain '" + stringify(os::PATH_SEPARATOR) + "'");
  }

  foreach (const csi::v0::VolumeCapability& capability,
           request->volume_capabilities()) {
    if (capability != defaultVolumeCapability) {
      return Status(grpc::INVALID_ARGUMENT, "Unsupported volume capabilities");
    }
  }

  if (request->parameters() != createParameters) {
    return Status(grpc::INVALID_ARGUMENT, "Unsupported create parameters");
  }

  // The volume ID is determined by `name`, so we check whether the volume
  // corresponding to `name` is compatible to the request if it exists.
  if (volumes.contains(request->name())) {
    const VolumeInfo volumeInfo = volumes.at(request->name());

    if (request->has_capacity_range()) {
      const csi::v0::CapacityRange& range = request->capacity_range();

      if (range.limit_bytes() != 0 &&
          volumeInfo.size > Bytes(range.limit_bytes())) {
        return Status(grpc::ALREADY_EXISTS, "Cannot satisfy 'limit_bytes'");
      } else if (range.required_bytes() != 0 &&
                 volumeInfo.size < Bytes(range.required_bytes())) {
        return Status(grpc::ALREADY_EXISTS, "Cannot satisfy 'required_bytes'");
      }
    }
  } else {
    if (availableCapacity == Bytes(0)) {
      return Status(grpc::OUT_OF_RANGE, "Insufficient capacity");
    }

    VolumeInfo volumeInfo;
    volumeInfo.id = request->name();
    volumeInfo.size = min(DEFAULT_VOLUME_CAPACITY, availableCapacity);

    if (request->has_capacity_range()) {
      const csi::v0::CapacityRange& range = request->capacity_range();

      // The highest we can pick.
      Bytes limit = range.limit_bytes() != 0
        ? min(availableCapacity, Bytes(range.limit_bytes()))
        : availableCapacity;

      if (range.required_bytes() != 0 &&
          limit < Bytes(range.required_bytes())) {
        return Status(grpc::OUT_OF_RANGE, "Cannot satisfy 'required_bytes'");
      }

      volumeInfo.size = min(
          limit,
          max(DEFAULT_VOLUME_CAPACITY, Bytes(range.required_bytes())));
    }

    const string path = getVolumePath(volumeInfo);

    Try<Nothing> mkdir = os::mkdir(path);
    if (mkdir.isError()) {
      return Status(
          grpc::INTERNAL,
          "Failed to create volume '" + volumeInfo.id + "': " + mkdir.error());
    }

    CHECK_GE(availableCapacity, volumeInfo.size);
    availableCapacity -= volumeInfo.size;
    volumes.put(volumeInfo.id, std::move(volumeInfo));
  }

  const VolumeInfo& volumeInfo = volumes.at(request->name());

  response->mutable_volume()->set_id(volumeInfo.id);
  response->mutable_volume()->set_capacity_bytes(volumeInfo.size.bytes());
  (*response->mutable_volume()->mutable_attributes())["path"] =
    getVolumePath(volumeInfo);

  return Status::OK;
}


Status TestCSIPlugin::DeleteVolume(
    ServerContext* context,
    const csi::v0::DeleteVolumeRequest* request,
    csi::v0::DeleteVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status::OK;
  }

  const VolumeInfo& volumeInfo = volumes.at(request->volume_id());
  const string path = getVolumePath(volumeInfo);

  Try<Nothing> rmdir = os::rmdir(path);
  if (rmdir.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to delete volume '" + request->volume_id() + "': " +
        rmdir.error());
  }

  availableCapacity += volumeInfo.size;
  volumes.erase(volumeInfo.id);

  return Status::OK;
}


Status TestCSIPlugin::ControllerPublishVolume(
    ServerContext* context,
    const csi::v0::ControllerPublishVolumeRequest* request,
    csi::v0::ControllerPublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const VolumeInfo& volumeInfo = volumes.at(request->volume_id());
  const string path = getVolumePath(volumeInfo);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  if (request->node_id() != NODE_ID) {
    return Status(
        grpc::NOT_FOUND,
        "Node '" + request->node_id() + "' is not found");
  }

  // Do nothing.
  return Status::OK;
}


Status TestCSIPlugin::ControllerUnpublishVolume(
    ServerContext* context,
    const csi::v0::ControllerUnpublishVolumeRequest* request,
    csi::v0::ControllerUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  if (request->node_id() != NODE_ID) {
    return Status(
        grpc::NOT_FOUND,
        "Node '" + request->node_id() + "' is not found");
  }

  // Do nothing.
  return Status::OK;
}


Status TestCSIPlugin::ValidateVolumeCapabilities(
    ServerContext* context,
    const csi::v0::ValidateVolumeCapabilitiesRequest* request,
    csi::v0::ValidateVolumeCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const VolumeInfo& volumeInfo = volumes.at(request->volume_id());
  const string path = getVolumePath(volumeInfo);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  foreach (const csi::v0::VolumeCapability& capability,
           request->volume_capabilities()) {
    if (capability != defaultVolumeCapability) {
      response->set_supported(false);
      response->set_message("Unsupported volume capabilities");

      return Status::OK;
    }
  }

  // TODO(chhsiao): Validate the parameters once we get CSI v1.

  response->set_supported(true);

  return Status::OK;
}


Status TestCSIPlugin::ListVolumes(
    ServerContext* context,
    const csi::v0::ListVolumesRequest* request,
    csi::v0::ListVolumesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Support the `max_entries` field.
  if (request->max_entries() > 0) {
    return Status(grpc::ABORTED, "Field 'max_entries' is not supported");
  }

  // TODO(chhsiao): Support the `starting_token` fields.
  if (!request->starting_token().empty()) {
    return Status(grpc::ABORTED, "Field 'starting_token' is not supported");
  }

  foreachvalue (const VolumeInfo& volumeInfo, volumes) {
    csi::v0::Volume* volume = response->add_entries()->mutable_volume();
    volume->set_id(volumeInfo.id);
    volume->set_capacity_bytes(volumeInfo.size.bytes());
    (*volume->mutable_attributes())["path"] = getVolumePath(volumeInfo);
  }

  return Status::OK;
}


Status TestCSIPlugin::GetCapacity(
    ServerContext* context,
    const csi::v0::GetCapacityRequest* request,
    csi::v0::GetCapacityResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  foreach (const csi::v0::VolumeCapability& capability,
           request->volume_capabilities()) {
    // We report zero capacity for any capability other than the
    // default-constructed mount volume capability since this plugin
    // does not support any filesystem types and mount flags.
    if (capability != defaultVolumeCapability) {
      response->set_available_capacity(0);

      return Status::OK;
    }
  }

  if (request->parameters() != createParameters) {
      response->set_available_capacity(0);

      return Status::OK;
  }

  response->set_available_capacity(availableCapacity.bytes());

  return Status::OK;
}


Status TestCSIPlugin::ControllerGetCapabilities(
    ServerContext* context,
    const csi::v0::ControllerGetCapabilitiesRequest* request,
    csi::v0::ControllerGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v0::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v0::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v0::ControllerServiceCapability::RPC::GET_CAPACITY);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::v0::ControllerServiceCapability::RPC::LIST_VOLUMES);

  return Status::OK;
}


Status TestCSIPlugin::NodeStageVolume(
    ServerContext* context,
    const csi::v0::NodeStageVolumeRequest* request,
    csi::v0::NodeStageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const VolumeInfo& volumeInfo = volumes.at(request->volume_id());
  const string path = getVolumePath(volumeInfo);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  if (!os::exists(request->staging_target_path())) {
    return Status(
        grpc::INVALID_ARGUMENT,
        "Target path '" + request->staging_target_path() + "' is not found");
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to get mount table: " + table.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->staging_target_path()) {
      return Status::OK;
    }
  }

  Try<Nothing> mount = fs::mount(
      path,
      request->staging_target_path(),
      None(),
      MS_BIND,
      None());

  if (mount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to mount from '" + path + "' to '" +
        request->staging_target_path() + "': " + mount.error());
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnstageVolume(
    ServerContext* context,
    const csi::v0::NodeUnstageVolumeRequest* request,
    csi::v0::NodeUnstageVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to get mount table: " + table.error());
  }

  bool found = false;
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->staging_target_path()) {
      found = true;
      break;
    }
  }

  if (!found) {
    return Status::OK;
  }

  Try<Nothing> unmount = fs::unmount(request->staging_target_path());
  if (unmount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to unmount '" + request->staging_target_path() +
        "': " + unmount.error());
  }

  return Status::OK;
}


Status TestCSIPlugin::NodePublishVolume(
    ServerContext* context,
    const csi::v0::NodePublishVolumeRequest* request,
    csi::v0::NodePublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const VolumeInfo& volumeInfo = volumes.at(request->volume_id());
  const string path = getVolumePath(volumeInfo);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  if (!os::exists(request->target_path())) {
    return Status(
        grpc::INVALID_ARGUMENT,
        "Target path '" + request->target_path() + "' is not found");
  }

  if (request->staging_target_path().empty()) {
    return Status(
        grpc::FAILED_PRECONDITION,
        "Expecting 'staging_target_path' to be set");
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to get mount table: " + table.error());
  }

  bool found = false;
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->staging_target_path()) {
      found = true;
      break;
    }
  }

  if (!found) {
    return Status(
        grpc::FAILED_PRECONDITION,
        "Volume '" + request->volume_id() + "' has not been staged yet");
  }

  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->target_path()) {
      return Status::OK;
    }
  }

  Try<Nothing> mount = fs::mount(
      request->staging_target_path(),
      request->target_path(),
      None(),
      MS_BIND | (request->readonly() ? MS_RDONLY : 0),
      None());

  if (mount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to mount from '" + path + "' to '" +
        request->target_path() + "': " + mount.error());
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnpublishVolume(
    ServerContext* context,
    const csi::v0::NodeUnpublishVolumeRequest* request,
    csi::v0::NodeUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to get mount table: " + table.error());
  }

  bool found = false;
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->target_path()) {
      found = true;
      break;
    }
  }

  if (!found) {
    return Status::OK;
  }

  Try<Nothing> unmount = fs::unmount(request->target_path());
  if (unmount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to unmount '" + request->target_path() +
        "': " + unmount.error());
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


string TestCSIPlugin::getVolumePath(const VolumeInfo& volumeInfo)
{
  return path::join(
      workDir,
      strings::join("-", stringify(volumeInfo.size), volumeInfo.id));
}


Try<TestCSIPlugin::VolumeInfo> TestCSIPlugin::parseVolumePath(
    const string& path)
{
  size_t pos = path.find_first_of("-");
  if (pos == string::npos) {
    return Error("Cannot find the delimiter");
  }

  string bytesString = path.substr(0, path.find_first_of("-"));
  string id = path.substr(path.find_first_of("-") + 1);

  Try<Bytes> bytes = Bytes::parse(bytesString);
  if (bytes.isError()) {
    return Error("Failed to parse bytes: " + bytes.error());
  }

  VolumeInfo volumeInfo;
  volumeInfo.id = id;
  volumeInfo.size = bytes.get();

  return volumeInfo;
}


// Serves CSI calls from the given endpoint through forwarding the calls to
// another CSI endpoint and returning back the results.
class CSIProxy
{
public:
  CSIProxy(const string& _endpoint, const string& forward)
    : endpoint(_endpoint),
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
//                                                     Server-side
//        +-------------+             +-------------+ WriteAndFinish +---+
//        | INITIALIZED |             |  FINISHING  +----------------> X |
//        +------+------+             +------^------+                +---+
//   Server-side |                           | Client-side
//   RequestCall |        Server-side        | Finish (unary call)
//        +------v------+    Read     +------+------+
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
          continue;
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
          // clean up the call and move on to the next one.
          delete call;
          continue;
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

        // The call is completed so clean it up.
        delete call;

        break;
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
      Option<Error> error;

      if (values.size() != 1) {
        error = "Parameter keys must be unique";
      } else {
        createParameters.put(param, values[0]);
      }

      if (error.isSome()) {
        cerr << "Failed to parse the '--create_parameters' flags: "
             << error->message << endl;
        return EXIT_FAILURE;
      }
    }
  }

  hashmap<string, Bytes> volumes;

  if (flags.volumes.isSome()) {
    foreachpair (const string& name,
                 const vector<string>& capacities,
                 strings::pairs(flags.volumes.get(), ";", ":")) {
      Option<Error> error;

      if (strings::contains(name, stringify(os::PATH_SEPARATOR))) {
        error =
          "Volume name cannot contain '" + stringify(os::PATH_SEPARATOR) + "'";
      } else if (capacities.size() != 1) {
        error = "Volume name must be unique";
      } else if (volumes.contains(name)) {
        error = "Volume '" + name + "' already exists";
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
    CSIProxy proxy(flags.endpoint, flags.forward.get());

    proxy.run();
  } else {
    TestCSIPlugin plugin(
        flags.work_dir,
        flags.endpoint,
        flags.available_capacity,
        createParameters,
        volumes);

    plugin.run();
  }
}
