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

#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>

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

#include "csi/spec.hpp"
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
        "to the pre-existing volumes.");

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
  : public csi::Identity::Service,
    public csi::Controller::Service,
    public csi::Node::Service
{
public:
  TestCSIPlugin(
      const string& _workDir,
      const string& _endpoint,
      const csi::Version& _version,
      const Bytes& _availableCapacity,
      const hashmap<string, Bytes>& _volumes)
    : workDir(_workDir),
      endpoint(_endpoint),
      version(_version),
      availableCapacity(_availableCapacity)
  {
    // TODO(jieyu): Consider not using CHECKs here.
    Try<list<string>> paths = os::ls(workDir);
    CHECK_SOME(paths);

    foreach (const string& path, paths.get()) {
      Try<Volume> volume = parseVolumePath(path);
      CHECK_SOME(volume);

      CHECK(!volumes.contains(volume->id));
      volumes.put(volume->id, volume.get());
    }

    foreachpair (const string& name, const Bytes& capacity, _volumes) {
      if (volumes.contains(name)) {
        continue;
      }

      Volume volume;
      volume.id = name;
      volume.size = capacity;

      const string path = getVolumePath(volume);

      Try<Nothing> mkdir = os::mkdir(path);
      CHECK_SOME(mkdir);

      volumes.put(volume.id, volume);
    }

    ServerBuilder builder;
    builder.AddListeningPort(endpoint, InsecureServerCredentials());
    builder.RegisterService(static_cast<csi::Identity::Service*>(this));
    builder.RegisterService(static_cast<csi::Controller::Service*>(this));
    builder.RegisterService(static_cast<csi::Node::Service*>(this));
    server = builder.BuildAndStart();
  }

  void wait()
  {
    if (server) {
      server->Wait();
    }
  }

  virtual Status GetSupportedVersions(
      ServerContext* context,
      const csi::GetSupportedVersionsRequest* request,
      csi::GetSupportedVersionsResponse* response) override;

  virtual Status GetPluginInfo(
      ServerContext* context,
      const csi::GetPluginInfoRequest* request,
      csi::GetPluginInfoResponse* response) override;

  virtual Status CreateVolume(
      ServerContext* context,
      const csi::CreateVolumeRequest* request,
      csi::CreateVolumeResponse* response) override;

  virtual Status DeleteVolume(
      ServerContext* context,
      const csi::DeleteVolumeRequest* request,
      csi::DeleteVolumeResponse* response) override;

  virtual Status ControllerPublishVolume(
      ServerContext* context,
      const csi::ControllerPublishVolumeRequest* request,
      csi::ControllerPublishVolumeResponse* response) override;

  virtual Status ControllerUnpublishVolume(
      ServerContext* context,
      const csi::ControllerUnpublishVolumeRequest* request,
      csi::ControllerUnpublishVolumeResponse* response) override;

  virtual Status ValidateVolumeCapabilities(
      ServerContext* context,
      const csi::ValidateVolumeCapabilitiesRequest* request,
      csi::ValidateVolumeCapabilitiesResponse* response) override;

  virtual Status ListVolumes(
      ServerContext* context,
      const csi::ListVolumesRequest* request,
      csi::ListVolumesResponse* response) override;

  virtual Status GetCapacity(
      ServerContext* context,
      const csi::GetCapacityRequest* request,
      csi::GetCapacityResponse* response) override;

  virtual Status ControllerProbe(
      ServerContext* context,
      const csi::ControllerProbeRequest* request,
      csi::ControllerProbeResponse* response) override;

  virtual Status ControllerGetCapabilities(
      ServerContext* context,
      const csi::ControllerGetCapabilitiesRequest* request,
      csi::ControllerGetCapabilitiesResponse* response) override;

  virtual Status NodePublishVolume(
      ServerContext* context,
      const csi::NodePublishVolumeRequest* request,
      csi::NodePublishVolumeResponse* response) override;

  virtual Status NodeUnpublishVolume(
      ServerContext* context,
      const csi::NodeUnpublishVolumeRequest* request,
      csi::NodeUnpublishVolumeResponse* response) override;

  virtual Status GetNodeID(
      ServerContext* context,
      const csi::GetNodeIDRequest* request,
      csi::GetNodeIDResponse* response) override;

  virtual Status NodeProbe(
      ServerContext* context,
      const csi::NodeProbeRequest* request,
      csi::NodeProbeResponse* response) override;

  virtual Status NodeGetCapabilities(
      ServerContext* context,
      const csi::NodeGetCapabilitiesRequest* request,
      csi::NodeGetCapabilitiesResponse* response) override;

private:
  struct Volume
  {
    string id;
    Bytes size;
  };

  Option<Error> validateVersion(const csi::Version& _version);

  string getVolumePath(const Volume& volume);
  Try<Volume> parseVolumePath(const string& path);

  const string workDir;
  const string endpoint;
  const csi::Version version;

  Bytes availableCapacity;
  hashmap<string, Volume> volumes;

  unique_ptr<Server> server;
};


Status TestCSIPlugin::GetSupportedVersions(
    ServerContext* context,
    const csi::GetSupportedVersionsRequest* request,
    csi::GetSupportedVersionsResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  response->add_supported_versions()->CopyFrom(version);

  return Status::OK;
}


Status TestCSIPlugin::GetPluginInfo(
    ServerContext* context,
    const csi::GetPluginInfoRequest* request,
    csi::GetPluginInfoResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  response->set_name(PLUGIN_NAME);
  response->set_vendor_version(MESOS_VERSION);

  return Status::OK;
}


Status TestCSIPlugin::CreateVolume(
    ServerContext* context,
    const csi::CreateVolumeRequest* request,
    csi::CreateVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

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

    Volume volume;
    volume.id = request->name();
    volume.size = min(DEFAULT_VOLUME_CAPACITY, availableCapacity);

    if (request->has_capacity_range()) {
      const csi::CapacityRange& range = request->capacity_range();

      // The highest we can pick.
      Bytes limit = availableCapacity;
      if (range.limit_bytes() != 0) {
        limit = min(availableCapacity, Bytes(range.limit_bytes()));
      }

      if (range.required_bytes() != 0 &&
          range.required_bytes() > limit.bytes()) {
        return Status(grpc::OUT_OF_RANGE, "Cannot satisfy 'required_bytes'");
      }

      volume.size = min(
          limit,
          max(DEFAULT_VOLUME_CAPACITY, Bytes(range.required_bytes())));
    }

    const string path = getVolumePath(volume);

    Try<Nothing> mkdir = os::mkdir(path);
    if (mkdir.isError()) {
      return Status(
          grpc::INTERNAL,
          "Failed to create volume '" + volume.id + "': " + mkdir.error());
    }

    availableCapacity -= volume.size;
    volumes.put(volume.id, volume);
  }

  const Volume& volume = volumes.at(request->name());

  response->mutable_volume_info()->set_id(volume.id);
  response->mutable_volume_info()->set_capacity_bytes(volume.size.bytes());
  (*response->mutable_volume_info()->mutable_attributes())["path"] =
    getVolumePath(volume);

  if (alreadyExists) {
    return Status(
        grpc::ALREADY_EXISTS,
        "Volume with name '" + request->name() + "' already exists");
  }

  return Status::OK;
}


Status TestCSIPlugin::DeleteVolume(
    ServerContext* context,
    const csi::DeleteVolumeRequest* request,
    csi::DeleteVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const Volume& volume = volumes.at(request->volume_id());
  const string path = getVolumePath(volume);

  Try<Nothing> rmdir = os::rmdir(path);
  if (rmdir.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to delete volume '" + request->volume_id() + "': " +
        rmdir.error());
  }

  availableCapacity -= volume.size;
  volumes.erase(volume.id);

  return Status::OK;
}


Status TestCSIPlugin::ControllerPublishVolume(
    ServerContext* context,
    const csi::ControllerPublishVolumeRequest* request,
    csi::ControllerPublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const Volume& volume = volumes.at(request->volume_id());
  const string path = getVolumePath(volume);

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
    const csi::ControllerUnpublishVolumeRequest* request,
    csi::ControllerUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

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
    const csi::ValidateVolumeCapabilitiesRequest* request,
    csi::ValidateVolumeCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const Volume& volume = volumes.at(request->volume_id());
  const string path = getVolumePath(volume);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  foreach (const csi::VolumeCapability& capability,
           request->volume_capabilities()) {
    if (capability.has_mount() &&
        (!capability.mount().fs_type().empty() ||
         !capability.mount().mount_flags().empty())) {
      response->set_supported(false);
      response->set_message("Only default capability is supported");

      return Status::OK;
    }

    if (capability.access_mode().mode() !=
        csi::VolumeCapability::AccessMode::SINGLE_NODE_WRITER) {
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
    const csi::ListVolumesRequest* request,
    csi::ListVolumesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  // TODO(chhsiao): Support the `max_entries` field.
  if (request->max_entries() > 0) {
    return Status(grpc::ABORTED, "Field 'max_entries' is not supported");
  }

  // TODO(chhsiao): Support the `starting_token` fields.
  if (!request->starting_token().empty()) {
    return Status(grpc::ABORTED, "Field 'starting_token' is not supported");
  }

  foreachvalue (const Volume& volume, volumes) {
    csi::VolumeInfo* info = response->add_entries()->mutable_volume_info();
    info->set_id(volume.id);
    info->set_capacity_bytes(volume.size.bytes());
    (*info->mutable_attributes())["path"] = getVolumePath(volume);
  }

  return Status::OK;
}


Status TestCSIPlugin::GetCapacity(
    ServerContext* context,
    const csi::GetCapacityRequest* request,
    csi::GetCapacityResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  foreach (const csi::VolumeCapability& capability,
           request->volume_capabilities()) {
    if (!capability.has_mount()) {
      response->set_available_capacity(0);

      return Status::OK;
    }

    if (capability.access_mode().mode() !=
        csi::VolumeCapability::AccessMode::SINGLE_NODE_WRITER) {
      response->set_available_capacity(0);

      return Status::OK;
    }
  }

  response->set_available_capacity(availableCapacity.bytes());

  return Status::OK;
}


Status TestCSIPlugin::ControllerProbe(
    ServerContext* context,
    const csi::ControllerProbeRequest* request,
    csi::ControllerProbeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  return Status::OK;
}


Status TestCSIPlugin::ControllerGetCapabilities(
    ServerContext* context,
    const csi::ControllerGetCapabilitiesRequest* request,
    csi::ControllerGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  response->add_capabilities()->mutable_rpc()->set_type(
      csi::ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::ControllerServiceCapability::RPC::GET_CAPACITY);
  response->add_capabilities()->mutable_rpc()->set_type(
      csi::ControllerServiceCapability::RPC::LIST_VOLUMES);

  return Status::OK;
}


Status TestCSIPlugin::NodePublishVolume(
    ServerContext* context,
    const csi::NodePublishVolumeRequest* request,
    csi::NodePublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  // TODO(chhsiao): Validate required fields.

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  if (!volumes.contains(request->volume_id())) {
    return Status(
        grpc::NOT_FOUND,
        "Volume '" + request->volume_id() + "' is not found");
  }

  const Volume& volume = volumes.at(request->volume_id());
  const string path = getVolumePath(volume);

  auto it = request->volume_attributes().find("path");
  if (it == request->volume_attributes().end() || it->second != path) {
    return Status(grpc::INVALID_ARGUMENT, "Invalid volume attributes");
  }

  if (!os::exists(request->target_path())) {
    return Status(
        grpc::INVALID_ARGUMENT,
        "Target path '" + request->target_path() + "' is not found");
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to get mount table: " + table.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == request->target_path()) {
      return Status::OK;
    }
  }

  Try<Nothing> mount = fs::mount(
      path,
      request->target_path(),
      None(),
      MS_BIND,
      None());

  if (mount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to mount from '" + path + "' to '" +
        request->target_path() + "': " + mount.error());
  }

  if (request->readonly()) {
    mount = fs::mount(
        None(),
        request->target_path(),
        None(),
        MS_BIND | MS_RDONLY | MS_REMOUNT,
        None());

    return Status(
        grpc::INTERNAL,
        "Failed to mount '" + request->target_path() +
        "' as read only: " + mount.error());
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeUnpublishVolume(
    ServerContext* context,
    const csi::NodeUnpublishVolumeRequest* request,
    csi::NodeUnpublishVolumeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

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

  const Volume& volume = volumes.at(request->volume_id());
  const string path = getVolumePath(volume);

  Try<Nothing> unmount = fs::unmount(request->target_path());
  if (unmount.isError()) {
    return Status(
        grpc::INTERNAL,
        "Failed to unmount '" + request->target_path() +
        "': " + unmount.error());
  }

  return Status::OK;
}


Status TestCSIPlugin::GetNodeID(
    ServerContext* context,
    const csi::GetNodeIDRequest* request,
    csi::GetNodeIDResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  response->set_node_id(NODE_ID);

  return Status::OK;
}


Status TestCSIPlugin::NodeProbe(
    ServerContext* context,
    const csi::NodeProbeRequest* request,
    csi::NodeProbeResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  return Status::OK;
}


Status TestCSIPlugin::NodeGetCapabilities(
    ServerContext* context,
    const csi::NodeGetCapabilitiesRequest* request,
    csi::NodeGetCapabilitiesResponse* response)
{
  LOG(INFO) << request->GetDescriptor()->name() << " '" << *request << "'";

  Option<Error> error = validateVersion(request->version());
  if (error.isSome()) {
    return Status(grpc::INVALID_ARGUMENT, error->message);
  }

  return Status::OK;
}


Option<Error> TestCSIPlugin::validateVersion(const csi::Version& _version)
{
  if (version != _version) {
    return Error("Version " + stringify(_version) + " is not supported");
  }

  return None();
}


string TestCSIPlugin::getVolumePath(const Volume& volume)
{
  return path::join(
      workDir,
      strings::join("-", stringify(volume.size), volume.id));
}


Try<TestCSIPlugin::Volume> TestCSIPlugin::parseVolumePath(const string& path)
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

  Volume volume;
  volume.id = id;
  volume.size = bytes.get();

  return volume;
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

  csi::Version version;
  version.set_major(0);
  version.set_minor(1);
  version.set_patch(0);

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
        } else if (capacity.get() == 0) {
          error = "Volume capacity cannot be zero";
        } else {
          volumes.put(pair[0], capacity.get());
        }
      }

      if (error.isSome()) {
        cerr << "Failed to parse item '" << token << "' in 'volumes' flag: "
             << error->message;
        return EXIT_FAILURE;
      }
    }
  }

  unique_ptr<TestCSIPlugin> plugin(new TestCSIPlugin(
      flags.work_dir,
      flags.endpoint,
      version,
      flags.available_capacity,
      volumes));

  plugin->wait();

  return EXIT_SUCCESS;
}
