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

using std::string;
using std::list;

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace paths {

// The root directory where we keep the information of CNI networks that each
// container joins. The layout is as follows:
//   /var/run/mesos/isolators/network/cni/
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
constexpr char ROOT_DIR[] = "/var/run/mesos/isolators/network/cni";


string getContainerDir(const string& rootDir, const string& containerId);


string getNamespacePath(const string& rootDir, const string& containerId);


string getNetworkDir(
    const string& rootDir,
    const string& containerId,
    const string& networkName);


Try<list<string>> getNetworkNames(
    const string& rootDir,
    const string& containerId);


string getNetworkConfigPath(
    const string& rootDir,
    const string& containerId,
    const string& networkName);


string getInterfaceDir(
    const string& rootDir,
    const string& containerId,
    const string& networkName,
    const string& ifName);


Try<list<string>> getInterfaces(
    const string& rootDir,
    const string& containerId,
    const string& networkName);


string getNetworkInfoPath(
    const string& rootDir,
    const string& containerId,
    const string& networkName,
    const string& ifName);

} // namespace paths {
} // namespace cni {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_CNI_PATHS_HPP__
