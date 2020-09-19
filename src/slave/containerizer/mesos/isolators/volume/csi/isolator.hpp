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

#ifndef __VOLUME_CSI_ISOLATOR_HPP__
#define __VOLUME_CSI_ISOLATOR_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/csi_server.hpp"
#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/volume/csi/state.hpp"

namespace mesos {
namespace internal {
namespace slave {

class VolumeCSIIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      CSIServer* csiServer);

  ~VolumeCSIIsolatorProcess() override {};

  bool supportsNesting() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
  struct Mount
  {
    Volume volume;
    CSIVolume csiVolume;
    std::string target;
  };

  struct Info
  {
    Info (const hashset<CSIVolume>& _volumes)
      : volumes(_volumes) {}

    hashset<CSIVolume> volumes;
  };

  VolumeCSIIsolatorProcess(
      const Flags& _flags,
      CSIServer* _csiServer,
      const std::string& _rootDir)
  : ProcessBase(process::ID::generate("volume-csi-isolator")),
    flags(_flags),
    csiServer(_csiServer),
    rootDir(_rootDir) {}

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const std::vector<Mount>& mounts,
      const Option<std::string>& user,
      const std::vector<process::Future<std::string>>& futures);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const std::vector<process::Future<Nothing>>& futures);

  Try<Nothing> recoverContainer(const ContainerID& containerId);

  const Flags flags;
  CSIServer* csiServer;

  // CSI volume information root directory.
  const std::string rootDir;

  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __VOLUME_CSI_ISOLATOR_HPP__
