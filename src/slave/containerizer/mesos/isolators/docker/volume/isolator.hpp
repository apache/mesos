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

#ifndef __DOCKER_VOLUME_ISOLATOR_HPP__
#define __DOCKER_VOLUME_ISOLATOR_HPP__

#include <string>
#include <vector>

#include <process/owned.hpp>
#include <process/sequence.hpp>

#include <stout/hashmap.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/docker/volume/driver.hpp"
#include "slave/containerizer/mesos/isolators/docker/volume/paths.hpp"
#include "slave/containerizer/mesos/isolators/docker/volume/state.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The isolator is responsible for preparing volumes using docker
// volume driver APIs,
class DockerVolumeIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  // This allows the driver client to be mock for testing.
  static Try<mesos::slave::Isolator*> _create(
      const Flags& flags,
      const process::Owned<docker::volume::DriverClient>& client);

  ~DockerVolumeIsolatorProcess() override;

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
  struct Info
  {
    Info (const hashset<DockerVolume>& _volumes)
      : volumes(_volumes) {}

    hashset<DockerVolume> volumes;
  };

  DockerVolumeIsolatorProcess(
      const Flags& flags,
      const std::string& rootDir,
      const process::Owned<docker::volume::DriverClient>& client);

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const std::vector<std::string>& targets,
      const std::vector<Volume::Mode>& volumeModes,
      const Option<std::string>& user,
      const std::vector<process::Future<std::string>>& futures);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const std::vector<process::Future<Nothing>>& futures);

  Try<Nothing> _recover(const ContainerID& containerId);

  process::Future<std::string> mount(
      const std::string& driver,
      const std::string& name,
      const hashmap<std::string, std::string>& options);

  process::Future<std::string> _mount(
      const std::string& driver,
      const std::string& name,
      const hashmap<std::string, std::string>& options);

  process::Future<Nothing> unmount(
      const std::string& driver,
      const std::string& name);

  process::Future<Nothing> _unmount(
      const std::string& driver,
      const std::string& name);

  const Flags flags;
  const std::string rootDir;
  const process::Owned<docker::volume::DriverClient> client;

  hashmap<ContainerID, process::Owned<Info>> infos;

  // For a given volume, the docker volume isolator might be doing
  // mounting and unmounting simultaneously. The sequence can make
  // sure the order we issue them is the same order they are executed.
  hashmap<DockerVolume, process::Sequence> sequences;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_VOLUME_ISOLATOR_HPP__
