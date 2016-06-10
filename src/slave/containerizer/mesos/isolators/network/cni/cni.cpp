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

#include <iostream>
#include <list>
#include <set>

#include <process/io.hpp>
#include <process/pid.hpp>
#include <process/subprocess.hpp>

#include <stout/adaptor.hpp>
#include <stout/os.hpp>
#include <stout/net.hpp>

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/mesos/isolators/network/cni/cni.hpp"

namespace io = process::io;
namespace paths = mesos::internal::slave::cni::paths;
namespace spec = mesos::internal::slave::cni::spec;

using std::cerr;
using std::endl;
using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::tuple;
using std::vector;

using process::Failure;
using process::Future;
using process::NO_SETSID;
using process::Owned;
using process::PID;
using process::Subprocess;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> NetworkCniIsolatorProcess::create(const Flags& flags)
{
  // If both '--network_cni_plugins_dir' and '--network_cni_config_dir' are not
  // specified when operator starts agent, then the 'network/cni' isolator will
  // behave as follows:
  // 1. For the container without 'NetworkInfo.name' specified, 'network/cni'
  //    isolator will act as no-op, i.e., the container will just use agent host
  //    network namespace.
  // 2. For the container with 'NetworkInfo.name' specified, it will be
  //    rejected by the 'network/cni' isolator since it has not loaded any CNI
  //    plugins or network configurations.
  if (flags.network_cni_plugins_dir.isNone() &&
      flags.network_cni_config_dir.isNone()) {
    return new MesosIsolator(Owned<MesosIsolatorProcess>(
        new NetworkCniIsolatorProcess(
            flags,
            hashmap<string, NetworkConfigInfo>())));
  }

  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The 'network/cni' isolator requires root permissions");
  }

  if (flags.network_cni_plugins_dir.isNone() ||
      flags.network_cni_plugins_dir->empty()) {
    return Error("Missing required '--network_cni_plugins_dir' flag");
  }

  if (flags.network_cni_config_dir.isNone() ||
      flags.network_cni_config_dir->empty()) {
    return Error("Missing required '--network_cni_config_dir' flag");
  }

  if (!os::exists(flags.network_cni_plugins_dir.get())) {
    return Error(
        "The CNI plugin directory '" +
        flags.network_cni_plugins_dir.get() + "' does not exist");
  }

  if (!os::exists(flags.network_cni_config_dir.get())) {
    return Error(
        "The CNI network configuration directory '" +
        flags.network_cni_config_dir.get() + "' does not exist");
  }

  Try<list<string>> entries = os::ls(flags.network_cni_plugins_dir.get());
  if (entries.isError()) {
    return Error(
        "Unable to list the CNI plugin directory '" +
        flags.network_cni_plugins_dir.get() + "': " + entries.error());
  } else if (entries.get().size() == 0) {
    return Error(
        "The CNI plugin directory '" +
        flags.network_cni_plugins_dir.get() + "' is empty");
  }

  entries = os::ls(flags.network_cni_config_dir.get());
  if (entries.isError()) {
    return Error(
        "Unable to list the CNI network configuration directory '" +
        flags.network_cni_config_dir.get() + "': " + entries.error());
  }

  hashmap<string, NetworkConfigInfo> networkConfigs;
  foreach (const string& entry, entries.get()) {
    const string path = path::join(flags.network_cni_config_dir.get(), entry);

    // Ignore directory entries.
    if (os::stat::isdir(path)) {
      continue;
    }

    Try<string> read = os::read(path);
    if (read.isError()) {
      return Error(
          "Failed to read CNI network configuration file '" +
          path + "': " + read.error());
    }

    Try<spec::NetworkConfig> parse = spec::parseNetworkConfig(read.get());
    if (parse.isError()) {
      return Error(
          "Failed to parse CNI network configuration file '" +
          path + "': " + parse.error());
    }

    const spec::NetworkConfig& networkConfig = parse.get();
    const string& name = networkConfig.name();
    if (networkConfigs.contains(name)) {
      return Error(
          "Multiple CNI network configuration files have same name: " + name);
    }

    const string& type = networkConfig.type();
    string pluginPath = path::join(flags.network_cni_plugins_dir.get(), type);
    if (!os::exists(pluginPath)) {
      return Error(
          "Failed to find CNI plugin '" + pluginPath +
          "' used by CNI network configuration file '" + path + "'");
    }

    Try<os::Permissions> permissions = os::permissions(pluginPath);
    if (permissions.isError()) {
      return Error(
          "Failed to stat CNI plugin '" + pluginPath + "': " +
          permissions.error());
    } else if (!permissions.get().owner.x &&
               !permissions.get().group.x &&
               !permissions.get().others.x) {
      return Error(
          "The CNI plugin '" + pluginPath + "' used by CNI network"
          " configuration file '" + path + "' is not executable");
    }

    if (networkConfig.has_ipam()) {
      const string& ipamType = networkConfig.ipam().type();

      pluginPath = path::join(flags.network_cni_plugins_dir.get(), ipamType);
      if (!os::exists(pluginPath)) {
        return Error(
            "Failed to find CNI IPAM plugin '" + pluginPath +
            "' used by CNI network configuration file '" + path + "'");
      }

      permissions = os::permissions(pluginPath);
      if (permissions.isError()) {
        return Error(
            "Failed to stat CNI IPAM plugin '" + pluginPath + "': " +
            permissions.error());
      } else if (!permissions.get().owner.x &&
                 !permissions.get().group.x &&
                 !permissions.get().others.x) {
        return Error(
            "The CNI IPAM plugin '" + pluginPath + "' used by CNI network"
            " configuration file '" + path + "' is not executable");
      }
    }

    networkConfigs[name] = NetworkConfigInfo{path, networkConfig};
  }

  if (networkConfigs.size() == 0) {
    return Error(
        "Unable to find any valid CNI network configuration files under '" +
        flags.network_cni_config_dir.get() + "'");
  }

  // Create the CNI network information root directory if it does not exist.
  Try<Nothing> mkdir = os::mkdir(paths::ROOT_DIR);
  if (mkdir.isError()) {
    return Error(
        "Failed to create CNI network information root directory at '" +
        string(paths::ROOT_DIR) + "': " + mkdir.error());
  }

  Result<string> rootDir = os::realpath(paths::ROOT_DIR);
  if (!rootDir.isSome()) {
    return Error(
        "Failed to determine canonical path of CNI network information root"
        " directory '" + string(paths::ROOT_DIR) + "': " +
        (rootDir.isError() ? rootDir.error() : "No such file or directory"));
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  // Trying to find the mount entry that contains the CNI network
  // information root directory. We achieve that by doing a reverse
  // traverse of the mount table to find the first entry whose target
  // is a prefix of the CNI network information root directory.
  Option<fs::MountInfoTable::Entry> rootDirMount;
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table->entries)) {
    if (strings::startsWith(rootDir.get(), entry.target)) {
      rootDirMount = entry;
      break;
    }
  }

  // It's unlikely that we cannot find 'rootDirMount' because '/' is
  // always mounted and will be the 'rootDirMount' if no other mounts
  // found in between.
  if (rootDirMount.isNone()) {
    return Error(
        "Cannot find the mount containing CNI network information"
        " root directory");
  }

  // If 'rootDirMount' is a shared mount in its own peer group, then
  // we don't need to do anything. Otherwise, we need to do a self
  // bind mount of CNI network information root directory to make sure
  // it's a shared mount in its own peer group.
  bool bindMountNeeded = false;

  if (rootDirMount->shared().isNone()) {
    bindMountNeeded = true;
  } else {
    foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
      // Skip 'rootDirMount' and any mount underneath it. Also, we
      // skip those mounts whose targets are not the parent of the CNI
      // network information root directory because even if they are
      // in the same peer group as the CNI network information root
      // directory mount, it won't affect it.
      if (entry.id != rootDirMount->id &&
          !strings::startsWith(entry.target, rootDir.get()) &&
          entry.shared() == rootDirMount->shared() &&
          strings::startsWith(rootDir.get(), entry.target)) {
        bindMountNeeded = true;
        break;
      }
    }
  }

  if (bindMountNeeded) {
    if (rootDirMount->target != rootDir.get()) {
      // This is the case where the CNI network information root
      // directory mount does not exist in the mount table (e.g., a
      // new host running Mesos agent for the first time).
      LOG(INFO) << "Bind mounting '" << rootDir.get()
                << "' and making it a shared mount";

      // NOTE: Instead of using fs::mount to perform the bind mount,
      // we use the shell command here because the syscall 'mount'
      // does not update the mount table (i.e., /etc/mtab). In other
      // words, the mount will not be visible if the operator types
      // command 'mount'. Since this mount will still be presented
      // after all containers and the slave are stopped, it's better
      // to make it visible. It's OK to use the blocking os::shell
      // here because 'create' will only be invoked during
      // initialization.
      Try<string> mount = os::shell(
          "mount --bind %s %s && "
          "mount --make-private %s && "
          "mount --make-shared %s",
          rootDir->c_str(),
          rootDir->c_str(),
          rootDir->c_str(),
          rootDir->c_str());

      if (mount.isError()) {
        return Error(
            "Failed to bind mount '" + rootDir.get() +
            "' and make it a shared mount: " + mount.error());
      }
    } else {
      // This is the case where the CNI network information root
      // directory mount is in the mount table, but it's not a shared
      // mount in its own peer group (possibly due to agent crash
      // while preparing the CNI network information root directory
      // mount). It's safe to re-do the following.
      LOG(INFO) << "Making '" << rootDir.get() << "' a shared mount";

      Try<string> mount = os::shell(
          "mount --make-private %s && "
          "mount --make-shared %s",
          rootDir->c_str(),
          rootDir->c_str());

      if (mount.isError()) {
        return Error(
            "Failed to make '" + rootDir.get() +
            "' a shared mount: " + mount.error());
      }
    }
  }

  Result<string> pluginDir = os::realpath(flags.network_cni_plugins_dir.get());
  if (!pluginDir.isSome()) {
    return Error(
        "Failed to determine canonical path of CNI plugin directory '" +
        flags.network_cni_plugins_dir.get() + "': " +
        (pluginDir.isError()
          ? pluginDir.error()
          : "No such file or directory"));
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new NetworkCniIsolatorProcess(
          flags,
          networkConfigs,
          rootDir.get(),
          pluginDir.get())));
}


Future<Nothing> NetworkCniIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // If the `network/cni` isolator is providing network isolation to a
  // container its `rootDir`should always be set.  This property of
  // the isolator will not be set only if the operator does not
  // specify the '--network_cni_plugins_dir' and
  // '--network_cni_config_dir' flags at Agent startup. In this
  // particular case the `network/cni` isolator should be a no-op.
  if (rootDir.isNone()) {
    return Nothing();
  }

  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();

    Try<Nothing> recover = _recover(containerId, state);
    if (recover.isError()) {
      return Failure(
          "Failed to recover CNI network information for container " +
          stringify(containerId) + ": " + recover.error());
    }
  }

  Try<list<string>> entries = os::ls(rootDir.get());
  if (entries.isError()) {
    return Failure(
        "Unable to list CNI network information root directory '" +
        rootDir.get() + "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    ContainerID containerId;
    containerId.set_value(Path(entry).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    // Recover CNI network information for orphan container.
    Try<Nothing> recover = _recover(containerId);
    if (recover.isError()) {
      return Failure(
          "Failed to recover CNI network information for orphan container " +
          stringify(containerId) + ": " + recover.error());
    }

    // Known orphan containers will be cleaned up by containerizer
    // using the normal cleanup path. See MESOS-2367 for details.
    if (orphans.contains(containerId)) {
      continue;
    }

    LOG(INFO) << "Removing unknown orphaned container " << containerId;

    cleanup(containerId);
  }

  return Nothing();
}


Try<Nothing> NetworkCniIsolatorProcess::_recover(
    const ContainerID& containerId,
    const Option<ContainerState>& state)
{
  // NOTE: This method will add an 'Info' to 'infos' only if the container was
  // launched by the CNI isolator and joined CNI network(s), and cleanup _might_
  // be required for that container. If we're sure that the cleanup is not
  // required (e.g., the container's directory has been deleted), we won't add
  // an 'Info' to 'infos' and the corresponding 'cleanup' will be skipped.

  const string containerDir =
    paths::getContainerDir(rootDir.get(), containerId.value());

  if (!os::exists(containerDir)) {
    // This may occur in the following cases:
    //   1. Executor has exited and the isolator has removed the container
    //      directory in '_cleanup()' but agent dies before noticing this.
    //   2. Agent dies before the isolator creates the container directory
    //      in 'isolate()'.
    //   3. The container joined the host network.
    // For the above cases, we do not need to do anything since there is nothing
    // to clean up after agent restarts.
    return Nothing();
  }

  Try<list<string>> networkNames =
    paths::getNetworkNames(rootDir.get(), containerId.value());

  if (networkNames.isError()) {
    return Error("Failed to list CNI network names: " + networkNames.error());
  }

  hashmap<string, ContainerNetwork> containerNetworks;
  foreach (const string& networkName, networkNames.get()) {
    if (!networkConfigs.contains(networkName)) {
      return Error("Unknown CNI network name '" + networkName + "'");
    }

    Try<list<string>> interfaces = paths::getInterfaces(
        rootDir.get(),
        containerId.value(),
        networkName);

    if (interfaces.isError()) {
      return Error(
          "Failed to list interfaces for network '" + networkName +
          "': " + interfaces.error());
    }

    // It's likely that the slave crashes right after removing the interface
    // directory in '_detach' but before the 'containerDir' is removed in
    // '_cleanup'. In that case, the 'interfaces' here might be empty. We should
    // continue, rather than returning a failure here.
    if (interfaces->empty()) {
      continue;
    }

    // TODO(jieyu): Currently a container can have only one interface attached
    // to a CNI network.
    if (interfaces->size() != 1) {
      return Error(
          "More than one interfaces detected for network '" +
          networkName + "'");
    }

    ContainerNetwork containerNetwork;
    containerNetwork.networkName = networkName;
    containerNetwork.ifName = interfaces->front();

    if (state.isSome()) {
      foreach (const mesos::NetworkInfo& networkInfo,
               state->executor_info().container().network_infos()) {
        if (networkInfo.name() == networkName) {
          containerNetwork.networkInfo = networkInfo;
        }
      }
    }

    const string networkInfoPath = paths::getNetworkInfoPath(
        rootDir.get(),
        containerId.value(),
        containerNetwork.networkName,
        containerNetwork.ifName);

    if (!os::exists(networkInfoPath)) {
      // This may occur in the case that agent dies before the isolator
      // checkpoints the output of CNI plugin in '_attach()'.
      LOG(WARNING)
          << "The checkpointed CNI plugin output '" << networkInfoPath
          << "' for container " << containerId << " does not exist";

      containerNetworks.put(networkName, containerNetwork);
      continue;
    }

    // TODO(jieyu): Instead of returning Error here, we might want to just print
    // a WARNING and continue the recovery. This is because the slave might
    // crash while checkpointing the file, leaving a potentially corrupted file.
    // We don't want to fail the recovery if that happens.
    Try<string> read = os::read(networkInfoPath);
    if (read.isError()) {
      return Error(
          "Failed to read CNI network information file '" +
          networkInfoPath + "': " + read.error());
    }

    Try<spec::NetworkInfo> parse = spec::parseNetworkInfo(read.get());
    if (parse.isError()) {
      return Error(
          "Failed to parse CNI network information file '" +
          networkInfoPath + "': " + parse.error());
    }

    containerNetwork.cniNetworkInfo = parse.get();

    containerNetworks.put(networkName, containerNetwork);
  }

  // We add to 'infos' even if 'containerNetworks' is empty. This is because
  // it's likely that the slave crashed after removing all interface
  // directories but before it is able to unmount the namespace handle and
  // remove the container directory. In that case, we still rely on 'cleanup'
  // to clean it up.
  infos.put(containerId, Owned<Info>(new Info(containerNetworks)));

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> NetworkCniIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  const ExecutorInfo& executorInfo = containerConfig.executor_info();
  if (!executorInfo.has_container()) {
    return None();
  }

  if (executorInfo.container().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare CNI networks for a MESOS container");
  }

  if (executorInfo.container().network_infos_size() == 0) {
    return None();
  }

  int ifIndex = 0;
  hashset<string> networkNames;
  hashmap<string, ContainerNetwork> containerNetworks;
  foreach (const mesos::NetworkInfo& networkInfo,
           executorInfo.container().network_infos()) {
    if (!networkInfo.has_name()) {
      continue;
    }

    const string& name = networkInfo.name();
    if (!networkConfigs.contains(name)) {
      return Failure("Unknown CNI network '" + name + "'");
    }

    if (networkNames.contains(name)) {
      return Failure(
          "Attempted to join CNI network '" + name + "' multiple times");
    }

    networkNames.insert(name);

    ContainerNetwork containerNetwork;
    containerNetwork.networkName = name;
    containerNetwork.ifName = "eth" + stringify(ifIndex++);
    containerNetwork.networkInfo = networkInfo;

    containerNetworks.put(name, containerNetwork);
  }

  if (!containerNetworks.empty()) {
    if (containerConfig.has_rootfs()) {
      Owned<Info> info(new Info(containerNetworks, containerConfig.rootfs()));
      infos.put(containerId, info);
    } else {
      infos.put(containerId, Owned<Info>(new Info(containerNetworks)));
    }

    ContainerLaunchInfo launchInfo;

    // Reset the `LIBPROCESS_IP` in the environment variable, so that
    // the container binds to the IP address allocated by the CNI
    // plugin. See MESOS-3553 to understand why we need to reset the
    // `LIBPROCESS_IP`.
    Environment_Variable* env =
      launchInfo.mutable_environment()->add_variables();

    env->set_name("LIBPROCESS_IP");
    env->set_value("0.0.0.0");

    // This is only for test. For testing 'network/cni' isolator, we will
    // use a mock CNI plugin and a mock CNI network configuration file
    // which has "__MESOS_TEST__" as network name. The mock plugin will
    // not create a new network namespace for the container. The container
    // will be launched in the host's network namespace. The mock plugin
    // will return the host's IP address for this test container.
    if (networkNames.contains("__MESOS_TEST__")) {
      launchInfo.set_namespaces(CLONE_NEWNS | CLONE_NEWUTS);
    } else {
      launchInfo.set_namespaces(CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWUTS);
    }

    return launchInfo;
  }

  return None();
}


Future<Nothing> NetworkCniIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  // NOTE: We return 'Nothing()' here because some container might not
  // specify 'NetworkInfo.name' (i.e., wants to join the host
  // network). In that case, we don't create an Info struct.
  if (!infos.contains(containerId)) {
    return Nothing();
  }

  // If the `network/cni` isolator is providing network isolation to a
  // container its `rootDir` and `pluginDir` should always be set.
  // These properties of the isolator will not be set only if the
  // operator does not specify the '--network_cni_plugins_dir' and
  // '--network_cni_config_dir' flags at Agent startup.
  CHECK_SOME(rootDir);
  CHECK_SOME(pluginDir);

  // Create the container directory.
  const string containerDir =
    paths::getContainerDir(rootDir.get(), containerId.value());

  Try<Nothing> mkdir = os::mkdir(containerDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create the container directory at '" +
        containerDir + "': " + mkdir.error());
  }

  // Bind mount the network namespace handle of the process 'pid' to
  // /var/run/mesos/isolators/network/cni/<containerId>/ns to hold an extra
  // reference to the network namespace which will be released in '_cleanup'.
  const string source = path::join("/proc", stringify(pid), "ns", "net");
  const string target =
    paths::getNamespacePath(rootDir.get(), containerId.value());

  Try<Nothing> touch = os::touch(target);
  if (touch.isError()) {
    return Failure("Failed to create the bind mount point: " + touch.error());
  }

  Try<Nothing> mount = fs::mount(source, target, None(), MS_BIND, nullptr);
  if (mount.isError()) {
    return Failure(
        "Failed to mount the network namespace handle from '" +
        source + "' to '" + target + "': " + mount.error());
  }

  LOG(INFO) << "Bind mounted '" << source << "' to '" << target
            << "' for container " << containerId;

  // Invoke CNI plugin to attach container to CNI networks.
  list<Future<Nothing>> futures;
  foreachkey (const string& networkName,
              infos[containerId]->containerNetworks) {
    futures.push_back(attach(containerId, networkName, target));
  }

  // NOTE: Here, we wait for all 'attach()' to finish before returning
  // to make sure DEL on plugin is not called (via 'cleanup()') if some
  // ADD on plugin is still pending.
  return await(futures)
    .then(defer(
        PID<NetworkCniIsolatorProcess>(this),
        &NetworkCniIsolatorProcess::_isolate,
        containerId,
        pid,
        lambda::_1));
}


Future<Nothing> NetworkCniIsolatorProcess::_isolate(
    const ContainerID& containerId,
    pid_t pid,
    const list<Future<Nothing>>& attaches)
{
  vector<string> messages;
  foreach (const Future<Nothing>& attach, attaches) {
    if (!attach.isReady()) {
      messages.push_back(
          attach.isFailed() ? attach.failure() : "discarded");
    }
  }

  if (!messages.empty()) {
    return Failure(strings::join("\n", messages));
  }

  CHECK(infos.contains(containerId));

  const string containerDir =
    paths::getContainerDir(rootDir.get(), containerId.value());

  CHECK(os::exists(containerDir));

  // Create the network file.
  string hostsPath = path::join(containerDir, "hosts");
  string hostnamePath = path::join(containerDir, "hostname");
  string resolvPath = path::join(containerDir, "resolv.conf");

  // Update the `hostname` file.
  Try<Nothing> write = os::write(hostnamePath, stringify(containerId));
  if (write.isError()) {
    return Failure(
        "Failed to write the hostname to '" + hostnamePath +
        "': " + write.error());
  }

  // Update the `hosts` file.
  // TODO(jieyu): Currently we support only IPv4.
  stringstream hosts;

  hosts << "127.0.0.1 localhost" << endl;
  foreachvalue (const ContainerNetwork& network,
                infos[containerId]->containerNetworks) {
    // NOTE: Update /etc/hosts with hostname and IP address. In case
    // there are multiple IP addreses associated with the container we
    // pick the first one.
    if (network.cniNetworkInfo.isSome() && network.cniNetworkInfo->has_ip4()) {
      // IP are always stored in CIDR notation so need to retrieve the
      // address without the subnet mask.
      Try<net::IPNetwork> ip = net::IPNetwork::parse(
          network.cniNetworkInfo->ip4().ip(),
          AF_INET);

      if (ip.isError()) {
        return Failure(
            "Unable to parse the IP address " +
            network.cniNetworkInfo->ip4().ip() +
            " for the container: " + ip.error());
      }

      hosts << ip->address() << " " << containerId << endl;
      break;
    }
  }

  write = os::write(hostsPath, hosts.str());
  if (write.isError()) {
    return Failure(
        "Failed to write the 'hosts' file at '" +
        hostsPath + "': " + write.error());
  }

  // Update 'resolv.conf' with nameservers learned from IPAM. In case
  // IPAM has not specified a DNS then we set the container
  // 'resolv.conf' to be the same as the host 'resolv.conf'
  // ('/etc/resolv.conf').
  stringstream resolv;
  foreachvalue (const ContainerNetwork& network,
                infos[containerId]->containerNetworks) {
    if (network.cniNetworkInfo.isNone() || !network.cniNetworkInfo->has_dns()) {
      continue;
    }

    foreach (const string& nameserver,
             network.cniNetworkInfo->dns().nameservers()) {
      resolv << "nameserver " << nameserver << endl;
    }
  }

  // If `resolv` does not have any nameserver set `resolvPath` to
  // '/etc/resolv.conf'.
  if (resolv.str().size() == 0) {
    if (!os::exists("/etc/resolv.conf")){
      return Failure("Cannot find host /etc/resolv.conf");
    }

    resolvPath = "/etc/resolv.conf";

    LOG(INFO) << "Unable to find DNS nameservers for container "
              << containerId << ". Using host '/etc/resolv.conf'";
  } else {
    LOG(INFO) << "DNS nameservers for container " << containerId
              << " are:\n" << resolv.str();

    write = os::write(resolvPath, resolv.str());
    if (write.isError()) {
      return Failure(
          "Failed to write 'resolv.conf' file at '" +
          resolvPath + "': " + write.error());
    }
  }

  // Setup the required network files and the hostname in the
  // container's filesystem and UTS namespace.
  NetworkCniIsolatorSetup setup;
  setup.flags.pid = pid;
  setup.flags.hostname = stringify(containerId);
  setup.flags.rootfs = infos[containerId]->rootfs;
  setup.flags.etc_hosts_path = hostsPath;
  setup.flags.etc_hostname_path = hostnamePath;
  setup.flags.etc_resolv_conf = resolvPath;

  vector<string> argv(2);
  argv[0] = "mesos-containerizer";
  argv[1] = NetworkCniIsolatorSetup::NAME;

  Try<Subprocess> s = subprocess(
      path::join(flags.launcher_dir, "mesos-containerizer"),
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      NO_SETSID,
      setup.flags);

  if (s.isError()) {
    return Failure(
        "Failed to execute the setup helper subprocess: " + s.error());
  }

  return await(s->status(), io::read(s->err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>>& t) -> Future<Nothing> {
      Future<Option<int>> status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the setup helper subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the setup helper subprocess");
      }

      Future<string> err = std::get<1>(t);
      if (!err.isReady()) {
        return Failure(
            "Failed to read stderr from the helper subprocess: " +
            (err.isFailed() ? err.failure() : "discarded"));
      }

      if (status.get() != 0) {
        return Failure(
            "Failed to setup hostname and network files: " + err.get());
      }

      return Nothing();
    });
}


Future<Nothing> NetworkCniIsolatorProcess::attach(
    const ContainerID& containerId,
    const std::string& networkName,
    const std::string& netNsHandle)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->containerNetworks.contains(networkName));

  const ContainerNetwork& containerNetwork =
    infos[containerId]->containerNetworks[networkName];

  const string ifDir = paths::getInterfaceDir(
      rootDir.get(),
      containerId.value(),
      networkName,
      containerNetwork.ifName);

  Try<Nothing> mkdir = os::mkdir(ifDir);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create interface directory for the interface '" +
        containerNetwork.ifName + "' of the network '" +
        containerNetwork.networkName + "': "+ mkdir.error());
  }

  // Prepare environment variables for CNI plugin.
  map<string, string> environment;
  environment["CNI_COMMAND"] = "ADD";
  environment["CNI_CONTAINERID"] = containerId.value();
  environment["CNI_PATH"] = pluginDir.get();
  environment["CNI_IFNAME"] = containerNetwork.ifName;
  environment["CNI_NETNS"] = netNsHandle;

  // Some CNI plugins need to run "iptables" to set up IP Masquerade,
  // so we need to set the "PATH" environment variable so that the
  // plugin can locate the "iptables" executable file.
  Option<string> value = os::getenv("PATH");
  if (value.isSome()) {
    environment["PATH"] = value.get();
  } else {
    environment["PATH"] =
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  }

  const NetworkConfigInfo& networkConfig =
    networkConfigs[containerNetwork.networkName];

  // Invoke the CNI plugin.
  const string& plugin = networkConfig.config.type();
  Try<Subprocess> s = subprocess(
      path::join(pluginDir.get(), plugin),
      {plugin},
      Subprocess::PATH(networkConfig.path),
      Subprocess::PIPE(),
      Subprocess::PATH("/dev/null"),
      NO_SETSID,
      None(),
      environment);

  if (s.isError()) {
    return Failure(
        "Failed to execute the CNI plugin '" + plugin + "': " + s.error());
  }

  return await(s->status(), io::read(s->out().get()))
    .then(defer(
        PID<NetworkCniIsolatorProcess>(this),
        &NetworkCniIsolatorProcess::_attach,
        containerId,
        networkName,
        plugin,
        lambda::_1));
}


Future<Nothing> NetworkCniIsolatorProcess::_attach(
    const ContainerID& containerId,
    const string& networkName,
    const string& plugin,
    const tuple<Future<Option<int>>, Future<string>>& t)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->containerNetworks.contains(networkName));

  Future<Option<int>> status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the CNI plugin '" +
        plugin + "' subprocess: " +
        (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the CNI plugin '" + plugin + "' subprocess");
  }

  // CNI plugin will print result (in case of success) or error (in
  // case of failure) to stdout.
  Future<string> output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from the CNI plugin '" +
        plugin + "' subprocess: " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  if (status.get() != 0) {
    return Failure(
        "The CNI plugin '" + plugin + "' failed to attach container " +
        containerId.value() + " to CNI network '" + networkName +
        "': " + output.get());
  }

  // Parse the output of CNI plugin.
  Try<spec::NetworkInfo> parse = spec::parseNetworkInfo(output.get());
  if (parse.isError()) {
    return Failure(
        "Failed to parse the output of the CNI plugin '" +
        plugin + "': " + parse.error());
  }

  if (parse.get().has_ip4()) {
    LOG(INFO) << "Got assigned IPv4 address '" << parse.get().ip4().ip()
              << "' from CNI network '" << networkName
              << "' for container " << containerId;
  }

  if (parse.get().has_ip6()) {
    LOG(INFO) << "Got assigned IPv6 address '" << parse.get().ip6().ip()
              << "' from CNI network '" << networkName
              << "' for container " << containerId;
  }

  // Checkpoint the output of CNI plugin.
  // The destruction of the container cannot happen in the middle of
  // 'attach()' and '_attach()' because the containerizer will wait
  // for 'isolate()' to finish before destroying the container.
  ContainerNetwork& containerNetwork =
    infos[containerId]->containerNetworks[networkName];

  const string networkInfoPath = paths::getNetworkInfoPath(
      rootDir.get(),
      containerId.value(),
      networkName,
      containerNetwork.ifName);

  Try<Nothing> write = os::write(networkInfoPath, output.get());
  if (write.isError()) {
    return Failure(
        "Failed to checkpoint the output of CNI plugin'" +
        output.get() + "': " + write.error());
  }

  containerNetwork.cniNetworkInfo = parse.get();

  return Nothing();
}


Future<ContainerStatus> NetworkCniIsolatorProcess::status(
    const ContainerID& containerId)
{
  // TODO(jieyu): We don't create 'Info' struct for containers that
  // want to join the host network. Currently, we rely on the
  // slave/containerizer to set the IP addresses in ContainerStatus.
  // Consider returning the IP address of the slave here.
  if (!infos.contains(containerId)) {
    return ContainerStatus();
  }

  ContainerStatus status;
  foreachvalue (const ContainerNetwork& containerNetwork,
                infos[containerId]->containerNetworks) {
    CHECK_SOME(containerNetwork.networkInfo);

    // NOTE: 'cniNetworkInfo' is None() before 'isolate()' finishes.
    if (containerNetwork.cniNetworkInfo.isNone()) {
      continue;
    }

    mesos::NetworkInfo* networkInfo = status.add_network_infos();
    networkInfo->CopyFrom(containerNetwork.networkInfo.get());
    networkInfo->clear_ip_addresses();

    if (containerNetwork.cniNetworkInfo->has_ip4()) {
      // Remove prefix length from IP address.
      Try<net::IPNetwork> ip = net::IPNetwork::parse(
          containerNetwork.cniNetworkInfo->ip4().ip(), AF_INET);

      if (ip.isError()) {
        return Failure(
            "Unable to parse the IP address " +
            containerNetwork.cniNetworkInfo->ip4().ip() +
            " for the container: " + ip.error());
      }

      mesos::NetworkInfo::IPAddress* ipAddress =
        networkInfo->add_ip_addresses();
      ipAddress->set_protocol(mesos::NetworkInfo::IPv4);
      ipAddress->set_ip_address(stringify(ip->address()));
    }

    if (containerNetwork.cniNetworkInfo->has_ip6()) {
      mesos::NetworkInfo::IPAddress* ip = networkInfo->add_ip_addresses();
      ip->set_protocol(mesos::NetworkInfo::IPv6);
      ip->set_ip_address(containerNetwork.cniNetworkInfo->ip6().ip());
      // TODO(djosborne): Perform subnet strip on ipv6 addresses.
    }
  }

  return status;
}


Future<Nothing> NetworkCniIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // NOTE: We don't keep an Info struct if the container is on the host network,
  // or if during recovery, we found that the cleanup for this container is not
  // required anymore (e.g., cleanup is done already, but the slave crashed and
  // didn't realize that it's done).
  if (!infos.contains(containerId)) {
    return Nothing();
  }

  // Invoke CNI plugin to detach container from CNI networks.
  list<Future<Nothing>> futures;
  foreachkey (const string& networkName,
              infos[containerId]->containerNetworks) {
    futures.push_back(detach(containerId, networkName));
  }

  return await(futures)
    .then(defer(
        PID<NetworkCniIsolatorProcess>(this),
        &NetworkCniIsolatorProcess::_cleanup,
        containerId,
        lambda::_1));
}


Future<Nothing> NetworkCniIsolatorProcess::_cleanup(
    const ContainerID& containerId,
    const list<Future<Nothing>>& detaches)
{
  CHECK(infos.contains(containerId));

  vector<string> messages;
  foreach (const Future<Nothing>& detach, detaches) {
    if (!detach.isReady()) {
      messages.push_back(
          detach.isFailed() ? detach.failure() : "discarded");
    }
  }

  if (!messages.empty()) {
    return Failure(strings::join("\n", messages));
  }

  const string containerDir =
    paths::getContainerDir(rootDir.get(), containerId.value());

  const string target =
    paths::getNamespacePath(rootDir.get(), containerId.value());

  if (os::exists(target)) {
    Try<Nothing> unmount = fs::unmount(target);
    if (unmount.isError()) {
      return Failure(
          "Failed to unmount the network namespace handle '" +
          target + "': " + unmount.error());
    }

    LOG(INFO) << "Unmounted the network namespace handle '"
              << target << "' for container " << containerId;
  }

  Try<Nothing> rmdir = os::rmdir(containerDir);
  if (rmdir.isError()) {
    return Failure(
        "Failed to remove the container directory '" +
        containerDir + "': " + rmdir.error());
  }

  LOG(INFO) << "Removed the container directory '" << containerDir << "'";

  infos.erase(containerId);

  return Nothing();
}


Future<Nothing> NetworkCniIsolatorProcess::detach(
    const ContainerID& containerId,
    const std::string& networkName)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->containerNetworks.contains(networkName));

  const ContainerNetwork& containerNetwork =
    infos[containerId]->containerNetworks[networkName];

  // Prepare environment variables for CNI plugin.
  map<string, string> environment;
  environment["CNI_COMMAND"] = "DEL";
  environment["CNI_CONTAINERID"] = containerId.value();
  environment["CNI_PATH"] = pluginDir.get();
  environment["CNI_IFNAME"] = containerNetwork.ifName;
  environment["CNI_NETNS"] =
      paths::getNamespacePath(rootDir.get(), containerId.value());

  // Some CNI plugins need to run "iptables" to set up IP Masquerade, so we
  // need to set the "PATH" environment variable so that the plugin can locate
  // the "iptables" executable file.
  Option<string> value = os::getenv("PATH");
  if (value.isSome()) {
    environment["PATH"] = value.get();
  } else {
    environment["PATH"] =
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  }

  const NetworkConfigInfo& networkConfig = networkConfigs[networkName];

  // Invoke the CNI plugin.
  const string& plugin = networkConfig.config.type();
  Try<Subprocess> s = subprocess(
      path::join(pluginDir.get(), plugin),
      {plugin},
      Subprocess::PATH(networkConfig.path),
      Subprocess::PIPE(),
      Subprocess::PATH("/dev/null"),
      NO_SETSID,
      None(),
      environment);

  if (s.isError()) {
    return Failure(
        "Failed to execute the CNI plugin '" + plugin + "': " + s.error());
  }

  return await(s->status(), io::read(s->out().get()))
    .then(defer(
        PID<NetworkCniIsolatorProcess>(this),
        &NetworkCniIsolatorProcess::_detach,
        containerId,
        networkName,
        plugin,
        lambda::_1));
}


Future<Nothing> NetworkCniIsolatorProcess::_detach(
    const ContainerID& containerId,
    const std::string& networkName,
    const string& plugin,
    const tuple<Future<Option<int>>, Future<string>>& t)
{
  CHECK(infos.contains(containerId));
  CHECK(infos[containerId]->containerNetworks.contains(networkName));

  Future<Option<int>> status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the CNI plugin '" +
        plugin + "' subprocess: " +
        (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the CNI plugin '" + plugin + "' subprocess");
  }

  if (status.get() == 0) {
    const string ifDir = paths::getInterfaceDir(
        rootDir.get(),
        containerId.value(),
        networkName,
        infos[containerId]->containerNetworks[networkName].ifName);

    Try<Nothing> rmdir = os::rmdir(ifDir);
    if (rmdir.isError()) {
      return Failure(
          "Failed to remove interface directory '" +
          ifDir + "': " + rmdir.error());
    }

    return Nothing();
  }

  // CNI plugin will print result (in case of success) or error (in
  // case of failure) to stdout.
  Future<string> output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from the CNI plugin '" +
        plugin + "' subprocess: " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  return Failure(
      "The CNI plugin '" + plugin + "' failed to detach container "
      "from network '" + networkName + "': " + output.get());
}


// Implementation of subcommand to setup relevant network files and
// hostname in the container UTS and mount namespace.
const char* NetworkCniIsolatorSetup::NAME = "network-cni-setup";


NetworkCniIsolatorSetup::Flags::Flags()
{
  add(&pid, "pid", "PID of the container");

  add(&hostname, "hostname", "Hostname of the container");

  add(&rootfs,
      "rootfs",
      "Path to rootfs for the container on the host-file system");

  add(&etc_hosts_path,
      "etc_hosts_path",
      "Path in the host file system for 'hosts' file");

  add(&etc_hostname_path,
      "etc_hostname_path",
      "Path in the host file system for 'hostname' file");

  add(&etc_resolv_conf,
      "etc_resolv_conf",
      "Path in the host file system for 'resolv.conf'");
}


int NetworkCniIsolatorSetup::execute()
{
  // NOTE: This method has to be run in a new mount namespace.

  if (flags.help) {
    cerr << "Usage: " << name() << " [OPTIONS]" << endl
         << "Supported options:" << endl
         << flags.usage();
    return EXIT_SUCCESS;
  }

  if (flags.hostname.isNone()) {
    cerr << "Hostname not specified" << endl;
    return EXIT_FAILURE;
  }

  if (flags.pid.isNone()) {
    cerr << "Container PID not specified" << endl;
    return EXIT_FAILURE;
  }

  if (flags.etc_hosts_path.isNone()) {
    cerr << "Path to 'hosts' not specified" <<endl;
    return EXIT_FAILURE;
  } else if (!os::exists(flags.etc_hosts_path.get())) {
    cerr << "Unable to find '" << flags.etc_hosts_path.get() << "'" << endl;
    return EXIT_FAILURE;
  }

  if (flags.etc_hostname_path.isNone()) {
    cerr << "Path to 'hostname' not specified" << endl;
    return EXIT_FAILURE;
  } else if (!os::exists(flags.etc_hostname_path.get())) {
    cerr << "Unable to find '" << flags.etc_hostname_path.get() << "'" << endl;
    return EXIT_FAILURE;
  }

  if (flags.etc_resolv_conf.isNone()) {
    cerr << "Path to 'resolv.conf' not specified." << endl;
    return EXIT_FAILURE;
  } else if (!os::exists(flags.etc_resolv_conf.get())) {
    cerr << "Unable to find '" << flags.etc_resolv_conf.get() << "'" << endl;
    return EXIT_FAILURE;
  }

  // Enter the mount namespace.
  Try<Nothing> setns = ns::setns(flags.pid.get(), "mnt");
  if (setns.isError()) {
    cerr << "Failed to enter the mount namespace of pid "
         << flags.pid.get() << ": " << setns.error() << endl;
    return EXIT_FAILURE;
  }

  // Enter the UTS namespace.
  setns = ns::setns(flags.pid.get(), "uts");
  if (setns.isError()) {
    cerr << "Failed to enter the UTS namespace of pid "
         << flags.pid.get() << ": " << setns.error() << endl;
    return EXIT_FAILURE;
  }

  // Setup hostname in container's UTS namespace.
  Try<Nothing> setHostname = net::setHostname(flags.hostname.get());
  if (setHostname.isError()) {
    cerr << "Failed to set the hostname of the container to '"
         << flags.hostname.get() << "': " << setHostname.error() << endl;
    return EXIT_FAILURE;
  }

  LOG(INFO) << "Set hostname to '" << flags.hostname.get() << "'" << endl;

  // TODO(jieyu): Currently there seems to be a race between the
  // filesystem isolator and other isolators to execute the `isolate`
  // method. This results in the rootfs of the container not being
  // marked as slave + recursive which can result in the mounts in the
  // container mnt namespace propagating back into the host mnt
  // namespace. This is dangerous, since these mounts won't be cleared
  // in the host mnt namespace once the container mnt namespace is
  // destroyed (when the process dies). To avoid any leakage we mark
  // the root as a SLAVE recursively to avoid any propagation of
  // mounts in the container mnt namespace back into the host mnt
  // namespace.
  Try<Nothing> mount = fs::mount(
      None(),
      "/",
      None(),
      MS_SLAVE | MS_REC,
      nullptr);

  if (mount.isError()) {
    cerr << "Failed to mark `/` as a SLAVE mount: " << mount.error() << endl;
    return EXIT_FAILURE;
  }

  // Initialize the host path and container path for the set of files
  // that need to be setup in the container file system.
  hashmap<string, string> files;
  files["/etc/hosts"] = flags.etc_hosts_path.get();
  files["/etc/hostname"] = flags.etc_hostname_path.get();
  files["/etc/resolv.conf"] = flags.etc_resolv_conf.get();

  foreachpair (const string& file, const string& source, files) {
    // Do the bind mount in the host filesystem since no process in
    // the new network namespace should be seeing the original network
    // files from the host filesystem. This is also required by the
    // command executor since command executor will be launched with
    // rootfs of host filesystem and will later pivot to the rootfs of
    // the container filesystem, when launching the task.
    if (!os::exists(file)) {
      // NOTE: We just fail if the mount point does not exist on the
      // host filesystem because we don't want to pollute the host
      // filesystem.
      cerr << "Mount point '" << file << "' does not exist "
           << "on the host filesystem"<< endl;
      return EXIT_FAILURE;
    }

    mount = fs::mount(
        source,
        file,
        None(),
        MS_BIND,
        nullptr);

    if (mount.isError()) {
      cerr << "Failed to bind mount from '" << source << "' to '"
           << file << "': " << mount.error() << endl;
      return EXIT_FAILURE;
    }

    // Do the bind mount in the container filesystem.
    if (flags.rootfs.isSome()) {
      const string target = path::join(flags.rootfs.get(), file);
      if (!os::exists(target)) {
        // Create the mount point in the container filesystem.
        Try<Nothing> touch = os::touch(target);
        if (touch.isError()) {
          cerr << "Failed to create the mount point '" << target
               << "' in the container filesystem" << endl;
          return EXIT_FAILURE;
        }
      }

      mount = fs::mount(
          source,
          target,
          None(),
          MS_BIND,
          nullptr);

      if (mount.isError()) {
        cerr << "Failed to bind mount from '" << source << "' to '"
             << target << "': " << mount.error() << endl;
        return EXIT_FAILURE;
      }
    }
  }

  return EXIT_SUCCESS;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
