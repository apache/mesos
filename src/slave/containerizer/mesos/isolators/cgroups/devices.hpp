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

#ifndef __CGROUPS_DEVICES_ISOLATOR_HPP__
#define __CGROUPS_DEVICES_ISOLATOR_HPP__

#include <list>
#include <string>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// This isolator uses the cgroups devices subsystem to
// restrict access to devices in `/dev`. A small set of
// default devices are whitelisted upon container creation,
// and access to all other devices is restricted. It is
// assumed that other isolators will be used to allow / deny
// access to devices outside the default whitelist.
class CgroupsDevicesIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;
  };

  CgroupsDevicesIsolatorProcess(
      const Flags& _flags,
      const std::string& hierarchy);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  const Flags flags;

  // The path to the cgroups subsystem hierarchy root.
  const std::string hierarchy;

  // TODO(bmahler): Use Owned<Info>.
  hashmap<ContainerID, Info*> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_DEVICES_ISOLATOR_HPP__
