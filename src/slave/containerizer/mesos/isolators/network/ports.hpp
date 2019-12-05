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

#ifndef __NETWORK_PORTS_ISOLATOR_HPP__
#define __NETWORK_PORTS_ISOLATOR_HPP__

#include <stdint.h>

#include <string>
#include <vector>

#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/interval.hpp>
#include <stout/option.hpp>

#include "linux/routing/diagnosis/diagnosis.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The `network/ports` isolator provides isolation of TCP listener
// ports for tasks that share the host network namespace. It ensures
// that tasks listen only on ports for which they hold `ports` resources.
class NetworkPortsIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<hashmap<uint32_t, routing::diagnosis::socket::Info>>
    getListeningSockets();

  static Try<std::vector<uint32_t>> getProcessSockets(pid_t pid);

  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~NetworkPortsIsolatorProcess() override {}

  bool supportsNesting() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

  // Public only for testing.
  process::Future<Nothing> check(
      const hashmap<ContainerID, IntervalSet<uint16_t>>& listeners);

protected:
  void initialize() override;

private:
  NetworkPortsIsolatorProcess(
      bool _cniIsolatorEnabled,
      const Duration& _watchInterval,
      const bool& _enforcePortsEnabled,
      const std::string& _cgroupsRoot,
      const std::string& _freezerHierarchy,
      const Option<IntervalSet<uint16_t>>& isolatedPorts);

  struct Info
  {
    Option<IntervalSet<uint16_t>> allocatedPorts;
    Option<IntervalSet<uint16_t>> activePorts;
    process::Promise<mesos::slave::ContainerLimitation> limitation;
  };

  const bool cniIsolatorEnabled;
  const Duration watchInterval;
  const bool enforceContainerPorts;
  const std::string cgroupsRoot;
  const std::string freezerHierarchy;
  const Option<IntervalSet<uint16_t>> isolatedPorts;

  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NETWORK_PORTS_ISOLATOR_HPP__
