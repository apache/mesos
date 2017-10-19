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

#include "slave/containerizer/mesos/isolators/network/ports.hpp"

#include <process/id.hpp>

#include "common/protobuf_utils.hpp"
#include "common/values.hpp"

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::internal::values::rangesToIntervalSet;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> NetworkPortsIsolatorProcess::create(const Flags& flags)
{
  return new MesosIsolator(process::Owned<MesosIsolatorProcess>(
      new NetworkPortsIsolatorProcess()));
}


NetworkPortsIsolatorProcess::NetworkPortsIsolatorProcess()
  : ProcessBase(process::ID::generate("network-ports-isolator"))
{
}


bool NetworkPortsIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Nothing> NetworkPortsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const auto& state, states) {
    CHECK(!infos.contains(state.container_id()))
      << "Duplicate ContainerID " << state.container_id();

    infos.emplace(state.container_id(), Owned<Info>(new Info()));
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> NetworkPortsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos.emplace(containerId, Owned<Info>(new Info()));

  return update(containerId, containerConfig.resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<ContainerLimitation> NetworkPortsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure(
        "Failed to watch ports for unknown container " +
        stringify(containerId));
  }

  return infos.at(containerId)->limitation.future();
}


Future<Nothing> NetworkPortsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  Option<Value::Ranges> ports = resources.ports();
  if (ports.isSome()) {
    const Owned<Info>& info = infos.at(containerId);
    info->ports = rangesToIntervalSet<uint16_t>(ports.get()).get();
  }

  return Nothing();
}


Future<Nothing> NetworkPortsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    LOG(INFO) << "Ignoring cleanup for unknown container " << containerId;
    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
