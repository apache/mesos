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

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>

#include "csi/v0_volume_manager_process.hpp"

namespace http = process::http;

using std::string;
using std::vector;

using google::protobuf::Map;

using process::Failure;
using process::Future;
using process::ProcessBase;

using process::grpc::client::Runtime;

namespace mesos{
namespace csi {
namespace v0 {

VolumeManagerProcess::VolumeManagerProcess(
    const http::URL& agentUrl,
    const string& _rootDir,
    const CSIPluginInfo& _info,
    const hashset<Service> _services,
    const string& containerPrefix,
    const Option<string>& authToken,
    const Runtime& _runtime,
    Metrics* _metrics)
  : ProcessBase(process::ID::generate("csi-v0-volume-manager")),
    rootDir(_rootDir),
    info(_info),
    services(_services),
    runtime(_runtime),
    metrics(_metrics),
    serviceManager(new ServiceManager(
        agentUrl,
        rootDir,
        info,
        services,
        containerPrefix,
        authToken,
        runtime,
        metrics))
{
  // This should have been validated in `VolumeManager::create`.
  CHECK(!services.empty())
    << "Must specify at least one service for CSI plugin type '" << info.type()
    << "' and name '" << info.name() << "'";
}


Future<Nothing> VolumeManagerProcess::recover()
{
  return Failure("Unimplemented");
}


Future<vector<VolumeInfo>> VolumeManagerProcess::listVolumes()
{
  return Failure("Unimplemented");
}


Future<Bytes> VolumeManagerProcess::getCapacity(
    const types::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return Failure("Unimplemented");
}


Future<VolumeInfo> VolumeManagerProcess::createVolume(
    const string& name,
    const Bytes& capacity,
    const types::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return Failure("Unimplemented");
}


Future<Option<Error>> VolumeManagerProcess::validateVolume(
    const VolumeInfo& volumeInfo,
    const types::VolumeCapability& capability,
    const Map<string, string>& parameters)
{
  return Failure("Unimplemented");
}


Future<bool> VolumeManagerProcess::deleteVolume(const string& volumeId)
{
  return Failure("Unimplemented");
}


Future<Nothing> VolumeManagerProcess::attachVolume(const string& volumeId)
{
  return Failure("Unimplemented");
}


Future<Nothing> VolumeManagerProcess::detachVolume(const string& volumeId)
{
  return Failure("Unimplemented");
}


Future<Nothing> VolumeManagerProcess::publishVolume(const string& volumeId)
{
  return Failure("Unimplemented");
}


Future<Nothing> VolumeManagerProcess::unpublishVolume(const string& volumeId)
{
  return Failure("Unimplemented");
}


VolumeManager::VolumeManager(
    const http::URL& agentUrl,
    const string& rootDir,
    const CSIPluginInfo& info,
    const hashset<Service>& services,
    const string& containerPrefix,
    const Option<string>& authToken,
    const Runtime& runtime,
    Metrics* metrics)
  : process(new VolumeManagerProcess(
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
    const types::VolumeCapability& capability,
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
    const types::VolumeCapability& capability,
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
    const types::VolumeCapability& capability,
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


Future<Nothing> VolumeManager::publishVolume(const string& volumeId)
{
  return recovered
    .then(process::defer(
        process.get(), &VolumeManagerProcess::publishVolume, volumeId));
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
