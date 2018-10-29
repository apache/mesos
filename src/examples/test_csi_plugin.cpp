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

#include <csi/spec.hpp>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/rmdir.hpp>

#include "csi/utils.hpp"

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

using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;


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

    add(&Flags::volumes,
        "volumes",
        "Creates pre-existing volumes upon start-up. The volumes are\n"
        "specified as a semicolon-delimited list of name:capacity pairs.\n"
        "If a volume with the same name already exists, the pair will be\n"
        "ignored. (Example: 'volume1:1GB;volume2:2GB')");
  }

  string endpoint;
  string work_dir;
  Bytes available_capacity;
  Option<string> volumes;
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
      const hashmap<string, Bytes>& _volumes)
    : workDir(_workDir),
      endpoint(_endpoint),
      availableCapacity(_availableCapacity)
  {
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

    ServerBuilder builder;
    builder.AddListeningPort(endpoint, InsecureServerCredentials());
    builder.RegisterService(static_cast<csi::v0::Identity::Service*>(this));
    builder.RegisterService(static_cast<csi::v0::Controller::Service*>(this));
    builder.RegisterService(static_cast<csi::v0::Node::Service*>(this));
    server = builder.BuildAndStart();
  }

  void wait()
  {
    if (server) {
      server->Wait();
    }
  }

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
  hashmap<string, VolumeInfo> volumes;

  unique_ptr<Server> server;
};


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

  if (request->name().find_first_of(os::PATH_SEPARATOR) != string::npos) {
    return Status(grpc::INVALID_ARGUMENT, "Volume name cannot contain '/'");
  }

  bool alreadyExists = volumes.contains(request->name());

  if (!alreadyExists) {
    if (availableCapacity == Bytes(0)) {
      return Status(grpc::OUT_OF_RANGE, "Insufficient capacity");
    }

    VolumeInfo volumeInfo;
    volumeInfo.id = request->name();
    volumeInfo.size = min(DEFAULT_VOLUME_CAPACITY, availableCapacity);

    if (request->has_capacity_range()) {
      const csi::v0::CapacityRange& range = request->capacity_range();

      // The highest we can pick.
      Bytes limit = availableCapacity;
      if (range.limit_bytes() != 0) {
        limit = min(availableCapacity, Bytes(range.limit_bytes()));
      }

      if (range.required_bytes() != 0 &&
          static_cast<size_t>(range.required_bytes()) > limit.bytes()) {
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
    volumes.put(volumeInfo.id, volumeInfo);
  }

  const VolumeInfo& volumeInfo = volumes.at(request->name());

  response->mutable_volume()->set_id(volumeInfo.id);
  response->mutable_volume()->set_capacity_bytes(volumeInfo.size.bytes());
  (*response->mutable_volume()->mutable_attributes())["path"] =
    getVolumePath(volumeInfo);

  if (alreadyExists) {
    return Status(
        grpc::ALREADY_EXISTS,
        "Volume with name '" + request->name() + "' already exists");
  }

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
    if (capability.has_mount() &&
        (!capability.mount().fs_type().empty() ||
         !capability.mount().mount_flags().empty())) {
      response->set_supported(false);
      response->set_message("Only default capability is supported");

      return Status::OK;
    }

    if (capability.access_mode().mode() !=
        csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER) {
      response->set_supported(false);
      response->set_message("Access mode is not supported");

      return Status::OK;
    }
  }

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
    // default-constructed `MountVolume` capability since this plugin
    // does not support any filesystem types and mount flags.
    if (!capability.has_mount() ||
        !capability.mount().fs_type().empty() ||
        !capability.mount().mount_flags().empty()) {
      response->set_available_capacity(0);

      return Status::OK;
    }

    if (capability.access_mode().mode() !=
        csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER) {
      response->set_available_capacity(0);

      return Status::OK;
    }
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

  hashmap<string, Bytes> volumes;

  if (flags.volumes.isSome()) {
    foreach (const string& token, strings::tokenize(flags.volumes.get(), ";")) {
      vector<string> pair = strings::tokenize(token, ":");

      Option<Error> error;

      if (pair.size() != 2) {
        error = "Not a name:capacity pair";
      } else if (pair[0].empty()) {
        error = "Volume name cannot be empty";
      } else if (pair[0].find_first_of(os::PATH_SEPARATOR) != string::npos) {
        error = "Volume name cannot contain '/'";
      } else if (volumes.contains(pair[0])) {
        error = "Volume name must be unique";
      } else {
        Try<Bytes> capacity = Bytes::parse(pair[1]);
        if (capacity.isError()) {
          error = capacity.error();
        } else {
          volumes.put(pair[0], capacity.get());
        }
      }

      if (error.isSome()) {
        cerr << "Failed to parse item '" << token << "' in 'volumes' flag: "
             << error->message << endl;
        return EXIT_FAILURE;
      }
    }
  }

  // Terminate the plugin if the endpoint socket file already exists to simulate
  // an `EADDRINUSE` bind error.
  const string endpointPath = strings::remove("unix://", flags.endpoint);
  if (os::exists(endpointPath)) {
    cerr << "Failed to create endpoint '" << endpointPath << "': already exists"
         << endl;
    return EXIT_FAILURE;
  }

  unique_ptr<TestCSIPlugin> plugin(new TestCSIPlugin(
      flags.work_dir,
      flags.endpoint,
      flags.available_capacity,
      volumes));

  plugin->wait();

  return EXIT_SUCCESS;
}
