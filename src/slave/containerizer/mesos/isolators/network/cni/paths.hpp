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

#ifndef __ISOLATOR_CNI_PATHS_HPP__
#define __ISOLATOR_CNI_PATHS_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace paths {

// The root directory where we keep the information of CNI networks that each
// container joins. The layout is as follows:
//   /<work_dir|runtime_dir>/isolators/network/cni/
//    |- <ID of Container1>/
//    |  |-- ns -> /proc/<pid>/ns/net (bind mount)
//    |  |-- <Name of CNI network 1>/
//    |  |   |-- network.conf (JSON file to keep the CNI network configuration)
//    |  |   |-- ifname1/
//    |  |       |-- network.info (JSON file to keep the output of CNI plugin)
//    |  |-- <Name of CNI network 2>/
//    |  |   |-- network.conf
//    |      |-- ifname2/
//    |          |-- network.info
//    |-- <ID of ContainerID 2>/
//    | ...
constexpr char CNI_DIR[] = "isolators/network/cni";


std::string getCniRootDir(const Flags& flags);


std::string getContainerDir(
    const std::string& rootDir,
    const ContainerID& containerId);


std::string getNamespacePath(
    const std::string& rootDir,
    const ContainerID& containerId);


std::string getNetworkDir(
    const std::string& rootDir,
    const ContainerID& containerId,
    const std::string& networkName);


Try<std::list<std::string>> getNetworkNames(
    const std::string& rootDir,
    const ContainerID& containerId);


std::string getNetworkConfigPath(
    const std::string& rootDir,
    const ContainerID& containerId,
    const std::string& networkName);


std::string getInterfaceDir(
    const std::string& rootDir,
    const ContainerID& containerId,
    const std::string& networkName,
    const std::string& ifName);


Try<std::list<std::string>> getInterfaces(
    const std::string& rootDir,
    const ContainerID& containerId,
    const std::string& networkName);


std::string getNetworkInfoPath(
    const std::string& rootDir,
    const ContainerID& containerId,
    const std::string& networkName,
    const std::string& ifName);

} // namespace paths {
} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_CNI_PATHS_HPP__
