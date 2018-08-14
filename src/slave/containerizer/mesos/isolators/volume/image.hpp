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

#ifndef __VOLUME_IMAGE_ISOLATOR_HPP__
#define __VOLUME_IMAGE_ISOLATOR_HPP__

#include <string>
#include <vector>

#include <process/shared.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The volume image isolator is responsible for preparing image
// volumes for mesos container.
class VolumeImageIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      const process::Shared<Provisioner>& provisioner);

  ~VolumeImageIsolatorProcess() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

private:
  VolumeImageIsolatorProcess(
      const Flags& flags,
      const process::Shared<Provisioner>& provisioner);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const std::vector<std::string>& targets,
      const std::vector<Volume::Mode>& volumeModes,
      const std::vector<process::Future<ProvisionInfo>>& futures);

  const Flags flags;
  const process::Shared<Provisioner> provisioner;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __VOLUME_IMAGE_ISOLATOR_HPP__
