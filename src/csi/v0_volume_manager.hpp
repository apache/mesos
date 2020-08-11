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

#ifndef __CSI_V0_VOLUME_MANAGER_HPP__
#define __CSI_V0_VOLUME_MANAGER_HPP__

#include <string>
#include <vector>

#include <google/protobuf/map.h>

#include <mesos/mesos.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "csi/metrics.hpp"
#include "csi/service_manager.hpp"
#include "csi/state.hpp"
#include "csi/volume_manager.hpp"

namespace mesos {
namespace csi {
namespace v0 {

// Forward declarations.
class VolumeManagerProcess;


class VolumeManager : public csi::VolumeManager
{
public:
  VolumeManager(
      const std::string& rootDir,
      const CSIPluginInfo& info,
      const hashset<Service>& services,
      const process::grpc::client::Runtime& runtime,
      ServiceManager* serviceManager,
      Metrics* metrics,
      SecretResolver* secretResolver);

  // Since this class contains `Owned` members which should not but can be
  // copied, explicitly make this class non-copyable.
  //
  // TODO(chhsiao): Remove this once MESOS-5122 is fixed.
  VolumeManager(const VolumeManager&) = delete;
  VolumeManager& operator=(const VolumeManager&) = delete;

  ~VolumeManager() override;

  process::Future<Nothing> recover() override;

  process::Future<std::vector<VolumeInfo>> listVolumes() override;

  process::Future<Bytes> getCapacity(
      const Volume::Source::CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters)
    override;

  process::Future<VolumeInfo> createVolume(
      const std::string& name,
      const Bytes& capacity,
      const Volume::Source::CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters)
    override;

  process::Future<Option<Error>> validateVolume(
      const VolumeInfo& volumeInfo,
      const Volume::Source::CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters)
    override;

  process::Future<bool> deleteVolume(const std::string& volumeId) override;

  process::Future<Nothing> attachVolume(const std::string& volumeId) override;

  process::Future<Nothing> detachVolume(const std::string& volumeId) override;

  process::Future<Nothing> publishVolume(
      const std::string& volumeId,
      const Option<state::VolumeState>& volumeState = None()) override;

  process::Future<Nothing> unpublishVolume(
      const std::string& volumeId) override;

private:
  process::Owned<VolumeManagerProcess> process;
  process::Future<Nothing> recovered;
};

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_V0_VOLUME_MANAGER_HPP__
