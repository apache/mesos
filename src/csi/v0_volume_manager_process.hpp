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

#ifndef __CSI_VOLUME_MANAGER_PROCESS_HPP__
#define __CSI_VOLUME_MANAGER_PROCESS_HPP__

#include <string>
#include <vector>

#include <google/protobuf/map.h>

#include <mesos/mesos.hpp>

#include <mesos/csi/types.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "csi/metrics.hpp"
#include "csi/service_manager.hpp"
#include "csi/v0_volume_manager.hpp"
#include "csi/volume_manager.hpp"

namespace mesos {
namespace csi {
namespace v0 {


class VolumeManagerProcess : public process::Process<VolumeManagerProcess>
{
public:
  explicit VolumeManagerProcess(
      const process::http::URL& agentUrl,
      const std::string& _rootDir,
      const CSIPluginInfo& _info,
      const hashset<Service> _services,
      const std::string& containerPrefix,
      const Option<std::string>& authToken,
      const process::grpc::client::Runtime& _runtime,
      Metrics* _metrics);

  process::Future<Nothing> recover();

  process::Future<std::vector<VolumeInfo>> listVolumes();

  process::Future<Bytes> getCapacity(
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<VolumeInfo> createVolume(
      const std::string& name,
      const Bytes& capacity,
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<Option<Error>> validateVolume(
      const VolumeInfo& volumeInfo,
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<bool> deleteVolume(const std::string& volumeId);

  process::Future<Nothing> attachVolume(const std::string& volumeId);

  process::Future<Nothing> detachVolume(const std::string& volumeId);

  process::Future<Nothing> publishVolume(const std::string& volumeId);

  process::Future<Nothing> unpublishVolume(const std::string& volumeId);

private:
  const std::string rootDir;
  const CSIPluginInfo info;
  const hashset<Service> services;

  process::grpc::client::Runtime runtime;
  Metrics* metrics;
  process::Owned<ServiceManager> serviceManager;
};

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_VOLUME_MANAGER_PROCESS_HPP__
