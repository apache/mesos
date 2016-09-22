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

#include <process/id.hpp>
#include <process/subprocess.hpp>

#include <stout/subcommand.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/paths.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class NetworkCniIsolatorSetup;


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

  virtual bool supportsNesting();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

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

  struct ContainerNetwork
  {
    // CNI network name.
    std::string networkName;

    // Interface name.
    std::string ifName;

    // NetworkInfo copied from the ExecutorInfo.containerInfo.network_infos
    // in 'prepare()' and '_recover()'.
    Option<mesos::NetworkInfo> networkInfo;

    // Protobuf of CNI network information returned by CNI plugin.
    Option<cni::spec::NetworkInfo> cniNetworkInfo;
  };

  struct Info
  {
    Info (const hashmap<std::string, ContainerNetwork>& _containerNetworks,
          const Option<std::string> _rootfs = None())
      : containerNetworks (_containerNetworks),
        rootfs(_rootfs) {}

    // CNI network information keyed by network name.
    //
    // NOTE: For nested containers, since the container shares the
    // network namespace with the root container of its hierarchy,
    // this should simply be a copy of the `containerNetworks` of its
    // root container.
    hashmap<std::string, ContainerNetwork> containerNetworks;

    // Rootfs of the container file system. In case the container uses
    // the host file system, this will be `None`.
    Option<std::string> rootfs;
  };

  NetworkCniIsolatorProcess(
      const Flags& _flags,
      const hashmap<std::string, NetworkConfigInfo>& _networkConfigs,
      const Option<std::string>& _rootDir = None(),
      const Option<std::string>& _pluginDir = None())
    : ProcessBase(process::ID::generate("mesos-network-cni-isolator")),
      flags(_flags),
      networkConfigs(_networkConfigs),
      rootDir(_rootDir),
      pluginDir(_pluginDir) {}

  process::Future<Nothing> _isolate(
      const ContainerID& containerId,
      pid_t pid,
      const list<process::Future<Nothing>>& attaches);

  process::Future<Nothing> __isolate(
      const NetworkCniIsolatorSetup& setup);

  Try<Nothing> _recover(
      const ContainerID& containerId,
      const Option<mesos::slave::ContainerState>& state = None());

  process::Future<Nothing> attach(
      const ContainerID& containerId,
      const std::string& networkName,
      const std::string& netNsHandle);

  process::Future<Nothing> _attach(
      const ContainerID& containerId,
      const std::string& networkName,
      const std::string& plugin,
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>>& t);

  process::Future<Nothing> detach(
      const ContainerID& containerId,
      const std::string& networkName);

  process::Future<Nothing> _detach(
      const ContainerID& containerId,
      const std::string& networkName,
      const std::string& plugin,
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>>& t);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const std::list<process::Future<Nothing>>& detaches);

  const Flags flags;

  // CNI network configurations keyed by network name.
  hashmap<std::string, NetworkConfigInfo> networkConfigs;

  // CNI network information root directory.
  const Option<std::string> rootDir;

  // CNI plugins directory.
  const Option<std::string> pluginDir;

  // Information of CNI networks that each container joins.
  hashmap<ContainerID, process::Owned<Info>> infos;
};


// A subcommand to setup container hostname and mount the hosts,
// resolv.conf and hostname from the host file system into the
// container's file system.  The hostname needs to be setup in the
// container's UTS namespace, and the files need to be bind mounted in
// the container's mnt namespace.
class NetworkCniIsolatorSetup : public Subcommand
{
public:
  static const char* NAME;

  struct Flags : public flags::FlagsBase
  {
    Flags();

    Option<pid_t> pid;
    Option<std::string> hostname;
    Option<std::string> rootfs;
    Option<std::string> etc_hosts_path;
    Option<std::string> etc_hostname_path;
    Option<std::string> etc_resolv_conf;
    bool bind_host_files;
  };

  NetworkCniIsolatorSetup() : Subcommand(NAME) {}

  Flags flags;

protected:
  virtual int execute();
  virtual flags::FlagsBase* getFlags() { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NETWORK_CNI_ISOLATOR_HPP__
