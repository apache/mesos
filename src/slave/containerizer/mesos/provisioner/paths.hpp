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

#ifndef __PROVISIONER_PATHS_HPP__
#define __PROVISIONER_PATHS_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace provisioner {
namespace paths {

// The provisioner rootfs directory is as follows:
// <work_dir> ('--work_dir' flag)
// |-- provisioner
//     |-- containers
//         |-- <container_id>
//             |-- layers (paths to all layers to provision)
//             |-- backends
//                 |-- <backend> (copy, bind, etc.)
//                     |-- rootfses
//                         |-- <rootfs_id> (the rootfs)
//             |-- containers (nested containers)
//                 |-- <container_id>
//                     |-- layers (paths to all layers to provision)
//                     |-- backends
//                         |-- <backend> (copy, bind, etc.)
//                             |-- rootfses
//                                 |-- <rootfs_id> (the rootfs)
//
// There can be multiple backends due to the change of backend flags.
// Under each backend a rootfs is identified by the 'rootfs_id' which
// is a UUID.


constexpr char LAYERS_FILE[] = "layers";


// TODO(gilbert): rename this to `getContainerPath` for consistency.
std::string getContainerDir(
    const std::string& provisionerDir,
    const ContainerID& containerId);


std::string getLayersFilePath(
    const std::string& provisionerDir,
    const ContainerID& containerId);


std::string getContainerRootfsDir(
    const std::string& provisionerDir,
    const ContainerID& containerId,
    const std::string& backend,
    const std::string& rootfsId);


// Recursively "ls" the container directory and return a map of
// backend -> {rootfsId, ...}
Try<hashmap<std::string, hashset<std::string>>>
listContainerRootfses(
    const std::string& provisionerDir,
    const ContainerID& containerId);


// Return a set of container IDs.
Try<hashset<ContainerID>> listContainers(
    const std::string& provisionerDir);


std::string getBackendDir(
    const std::string& provisionerDir,
    const ContainerID& containerId,
    const std::string& backend);

} // namespace paths {
} // namespace provisioner {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_PATHS_HPP__
