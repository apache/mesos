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

#include "slave/containerizer/mesos/isolators/network/cni/paths.hpp"

#include <mesos/type_utils.hpp>

#include <stout/fs.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

using std::string;
using std::list;

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace paths {

string getCniRootDir(const Flags& flags)
{
  string workDir = flags.network_cni_root_dir_persist
    ? flags.work_dir
    : flags.runtime_dir;

  return path::join(workDir, paths::CNI_DIR);
}


string getContainerDir(
    const string& rootDir,
    const ContainerID& containerId)
{
  return path::join(rootDir, stringify(containerId));
}


string getNamespacePath(
    const string& rootDir,
    const ContainerID& containerId)
{
  return path::join(getContainerDir(rootDir, containerId), "ns");
}


string getNetworkDir(
    const string& rootDir,
    const ContainerID& containerId,
    const string& networkName)
{
  return path::join(getContainerDir(rootDir, containerId), networkName);
}


Try<list<string>> getNetworkNames(
    const string& rootDir,
    const ContainerID& containerId)
{
  const string& networkInfoDir = getContainerDir(rootDir, containerId);

  Try<list<string>> entries = os::ls(networkInfoDir);
  if (entries.isError()) {
    return Error(
        "Unable to list the CNI network information directory '" +
        networkInfoDir + "': " + entries.error());
  }

  list<string> networkNames;
  foreach (const string& entry, entries.get()) {
    const string path = path::join(networkInfoDir, entry);

    if (os::stat::isdir(path)) {
      networkNames.push_back(entry);
    }
  }

  return networkNames;
}


string getNetworkConfigPath(
    const string& rootDir,
    const ContainerID& containerId,
    const string& networkName)
{
  return path::join(
      getNetworkDir(rootDir, containerId, networkName),
      "network.conf");
}


string getInterfaceDir(
    const string& rootDir,
    const ContainerID& containerId,
    const string& networkName,
    const string& ifName)
{
  return path::join(getNetworkDir(rootDir, containerId, networkName), ifName);
}


Try<list<string>> getInterfaces(
    const string& rootDir,
    const ContainerID& containerId,
    const string& networkName)
{
  const string& networkDir = getNetworkDir(rootDir, containerId, networkName);

  Try<list<string>> entries = os::ls(networkDir);
  if (entries.isError()) {
    return Error(
        "Unable to list the CNI network directory '" + networkDir + "': " +
        entries.error());
  }

  list<string> ifNames;
  foreach (const string& entry, entries.get()) {
    const string path = path::join(networkDir, entry);

    if (os::stat::isdir(path)) {
      ifNames.push_back(entry);
    }
  }

  return ifNames;
}


string getNetworkInfoPath(
    const string& rootDir,
    const ContainerID& containerId,
    const string& networkName,
    const string& ifName)
{
  return path::join(
      getInterfaceDir(rootDir, containerId, networkName, ifName),
      "network.info");
}

} // namespace paths {
} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
