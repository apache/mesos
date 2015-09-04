/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MESOS_APPC_PATHS_HPP__
#define __MESOS_APPC_PATHS_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace appc {
namespace paths {

// The appc store file system layout is as follows:
//
// <store_dir> ('--appc_store_dir' flag)
// |--staging (contains temp directories for staging downloads)
// |
// |--images (stores validated images)
//    |--<image_id> (in the form of "sha512-<128_character_hash_sum>")
//       |--manifest
//       |--rootfs
//          |--... (according to the ACI spec)
//
// TODO(xujyan): The staging directory is unused for now (it's
// externally managed) but implemented to illustrate the need for a
// separate 'images' directory. Complete the layout diagram when the
// staging directory is utilized by the provisioner.
//
// The appc provisioner rootfs directory is as follows:
// <work_dir> ('--work_dir' flag)
// |-- provisioners
//     |-- APPC (see definition in src/slave/paths.hpp)
//         |-- containers
//             |-- <container_id>
//                 |-- backends
//                     |-- <backend> (copy, bind, etc.)
//                         |-- rootfses
//                             |-- <rootfs_id> (the rootfs)
//
// NOTE: Each container could have multiple image types, therefore there
// can be the same <container_id> directory under other provisioners e.g.,
// <work_dir>/provisioners/DOCKER, etc. Under each provisioner + container
// there can be multiple backends due to the change of backend flags. For
// appc, under each backend a rootfs is identified by the 'rootfs_id' which
// is a UUID.

std::string getStagingDir(const std::string& storeDir);


std::string getImagesDir(const std::string& storeDir);


std::string getImagePath(
    const std::string& storeDir,
    const std::string& imageId);


std::string getImageRootfsPath(
    const std::string& storeDir,
    const std::string& imageId);


std::string getImageRootfsPath(const std::string& imagePath);


std::string getImageManifestPath(
    const std::string& storeDir,
    const std::string& imageId);


std::string getImageManifestPath(const std::string& imagePath);


std::string getContainerRootfsDir(
    const std::string& provisionerDir,
    const ContainerID& containerId,
    const std::string& backend,
    const std::string& rootfsId);


// Recursively "ls" the container directory and return a map of
// backend -> rootfsId -> rootfsPath.
Try<hashmap<std::string, hashmap<std::string, std::string>>>
listContainerRootfses(
    const std::string& provisionerDir,
    const ContainerID& containerId);

// Return a map of containerId -> containerPath;
Try<hashmap<ContainerID, std::string>> listContainers(
    const std::string& provisionerDir);

} // namespace paths {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_APPC_PATHS__
