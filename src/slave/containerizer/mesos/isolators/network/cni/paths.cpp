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

#include <stout/path.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/paths.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace cni {
namespace paths {

string getNetworkInfoDir(const string& rootDir, const string& containerId)
{
  return path::join(rootDir, containerId);
}


string getNamespacePath(const string& rootDir, const string& containerId)
{
  return path::join(getNetworkInfoDir(rootDir, containerId), "ns");
}


string getNetworkDir(
    const string& rootDir,
    const string& containerId,
    const string& networkName)
{
  return path::join(getNetworkInfoDir(rootDir, containerId), networkName);
}


string getInterfaceDir(
    const string& rootDir,
    const string& containerId,
    const string& networkName,
    const string& ifName)
{
  return path::join(getNetworkDir(rootDir, containerId, networkName), ifName);
}


string getNetworkInfoPath(
    const string& rootDir,
    const string& containerId,
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
