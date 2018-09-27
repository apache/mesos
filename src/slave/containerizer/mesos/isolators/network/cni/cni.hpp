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

#include "linux/ns.hpp"

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

  ~NetworkCniIsolatorProcess() override {}

  bool supportsNesting() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) override;

  process::Future<ContainerStatus> status(
      const ContainerID& containerId) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
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
          const Option<std::string>& _rootfs = None(),
          const Option<std::string>& _hostname = None(),
          bool _joinsParentsNetwork = false)
      : containerNetworks (_containerNetworks),
        rootfs(_rootfs),
        hostname(_hostname),
        joinsParentsNetwork(_joinsParentsNetwork) {}

    // CNI network information keyed by network name.
    //
    // NOTE: For nested containers, since the container shares the
    // network namespace with the root container of its hierarchy,
    // this should simply be a copy of the `containerNetworks` of its
    // root container.
    hashmap<std::string, ContainerNetwork> containerNetworks;

    // Rootfs of the container file system. In case the container uses
    // the host file system, this will be `None`.
    const Option<std::string> rootfs;

    const Option<std::string> hostname;
    const bool joinsParentsNetwork;
  };

  // Reads each CNI config present in `configDir`, validates if the
  // `plugin` is present in the search path associated with
  // `pluginDir` and adds the CNI network config to `networkConfigs`
  // if the validation passes. If there is an error while reading the
  // CNI config, or if the plugin is not found, we log an error and the
  // CNI network config is not added to `networkConfigs`.
  static Try<hashmap<std::string, std::string>> loadNetworkConfigs(
      const std::string& configDir,
      const std::string& pluginDir);

  NetworkCniIsolatorProcess(
      const Flags& _flags,
      const hashmap<std::string, std::string>& _networkConfigs,
      const hashmap<std::string, ContainerDNSInfo::MesosInfo>& _cniDNSMap,
      const Option<ContainerDNSInfo::MesosInfo>& _defaultCniDNS = None(),
      const Option<std::string>& _rootDir = None())
    : ProcessBase(process::ID::generate("mesos-network-cni-isolator")),
      flags(_flags),
      networkConfigs(_networkConfigs),
      cniDNSMap(_cniDNSMap),
      defaultCniDNS(_defaultCniDNS),
      rootDir(_rootDir) {}

  process::Future<Nothing> _isolate(
      const ContainerID& containerId,
      pid_t pid,
      const std::vector<process::Future<Nothing>>& attaches);

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
          process::Future<std::string>,
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
          process::Future<std::string>,
          process::Future<std::string>>& t);

  process::Future<Nothing> _cleanup(
      const ContainerID& containerId,
      const std::vector<process::Future<Nothing>>& detaches);

  static Try<ResourceStatistics> _usage(
      const hashset<std::string> ifNames);

  // Searches the `networkConfigs` hashmap for a CNI network. If the
  // hashmap doesn't contain the network, will try to load all the CNI
  // configs from `flags.network_cni_config_dir`, and will then
  // perform another search of the `networkConfigs` hashmap to see if
  // the missing network was present on disk.
  Try<JSON::Object> getNetworkConfigJSON(const std::string& network);

  // Given a network name and the path for the CNI network
  // configuration file, reads the file, parses the JSON and
  // validates the name of the network to which this configuration
  // file belongs.
  Try<JSON::Object> getNetworkConfigJSON(
      const std::string& network,
      const std::string& path);

  const Flags flags;

  // A map storing the path to CNI network configuration files keyed
  // by the network name.
  hashmap<std::string, std::string> networkConfigs;

  // DNS informations of CNI networks keyed by CNI network name.
  hashmap<std::string, ContainerDNSInfo::MesosInfo> cniDNSMap;

  // Default DNS information for all CNI networks.
  const Option<ContainerDNSInfo::MesosInfo> defaultCniDNS;

  // CNI network information root directory.
  const Option<std::string> rootDir;

  // Information of CNI networks that each container joins.
  hashmap<ContainerID, process::Owned<Info>> infos;

  // Runner manages a separate thread to call `usage` functions
  // in the containers' namespaces.
  ns::NamespaceRunner namespaceRunner;
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

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<pid_t> pid;
    Option<std::string> hostname;
    Option<std::string> rootfs;
    Option<std::string> etc_hosts_path;
    Option<std::string> etc_hostname_path;
    Option<std::string> etc_resolv_conf;
    bool bind_host_files;
    bool bind_readonly;
  };

  NetworkCniIsolatorSetup() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NETWORK_CNI_ISOLATOR_HPP__
