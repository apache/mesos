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

#ifndef __POSIX_FILESYSTEM_ISOLATOR_HPP__
#define __POSIX_FILESYSTEM_ISOLATOR_HPP__

#include <mesos/resources.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/volume_gid_manager/volume_gid_manager.hpp"

namespace mesos {
namespace internal {
namespace slave {

class PosixFilesystemIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      VolumeGidManager* volumeGidManager);

  ~PosixFilesystemIsolatorProcess() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

protected:
  PosixFilesystemIsolatorProcess(
      const Flags& flags,
      VolumeGidManager* volumeGidManager);

  const Flags flags;
  VolumeGidManager* volumeGidManager;

  struct Info
  {
    explicit Info(const std::string& _directory)
      : directory(_directory) {}

    const std::string directory;

    // Track resources so we can unlink unneeded persistent volumes.
    Resources resources;

    std::vector<gid_t> gids;
  };

  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __POSIX_FILESYSTEM_ISOLATOR_HPP__
