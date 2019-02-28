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

#ifndef __VOLUME_SANDBOX_PATH_ISOLATOR_HPP__
#define __VOLUME_SANDBOX_PATH_ISOLATOR_HPP__

#include <string>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

#include "slave/volume_gid_manager/volume_gid_manager.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

class VolumeSandboxPathIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      VolumeGidManager* volumeGidManager);

  ~VolumeSandboxPathIsolatorProcess() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
  VolumeSandboxPathIsolatorProcess(
      const Flags& flags,
#ifdef __linux__
      VolumeGidManager* volumeGidManager,
#endif // __linux__
      bool bindMountSupported);

  const Flags flags;

#ifdef __linux__
  VolumeGidManager* volumeGidManager;
#endif // __linux__

  const bool bindMountSupported;

  hashmap<ContainerID, std::string> sandboxes;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __VOLUME_SANDBOX_PATH_ISOLATOR_HPP__
