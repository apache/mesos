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

#ifndef __NETWORK_CNI_ISOLATOR_HPP__
#define __NETWORK_CNI_ISOLATOR_HPP__

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"

namespace mesos {
namespace internal {
namespace slave {

// This isolator implements support for Container Network Interface (CNI)
// specification <https://github.com/appc/cni/blob/master/SPEC.md> . It
// provides network isolation to containers by creating a network namespace
// for each container, and then adding the container to the CNI network
// specified in the NetworkInfo for the container. It adds the container to
// a CNI network by using CNI plugins specified by the operator for the
// corresponding CNI network.
class NetworkCniIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual ~NetworkCniIsolatorProcess() {}

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  struct NetworkConfigInfo
  {
    // Path to CNI network configuration file.
    std::string path;

    // Protobuf of CNI network configuration.
    cni::spec::NetworkConfig config;
  };

  NetworkCniIsolatorProcess(
      const Flags& _flags,
      const hashmap<std::string, NetworkConfigInfo>& _networkConfigs)
    : flags(_flags),
      networkConfigs(_networkConfigs) {}

  const Flags flags;

  // CNI network configurations keyed by network name.
  hashmap<std::string, NetworkConfigInfo> networkConfigs;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NETWORK_CNI_ISOLATOR_HPP__
