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

#ifndef __PROVISIONER_APPC_PATHS_HPP__
#define __PROVISIONER_APPC_PATHS_HPP__

#include <string>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include <mesos/appc/spec.hpp>

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

std::string getStagingDir(const std::string& storeDir);


std::string getImagesDir(const std::string& storeDir);


std::string getImagePath(
    const std::string& storeDir,
    const std::string& imageId);


std::string getImageRootfsPath(
    const std::string& storeDir,
    const std::string& imageId);


std::string getImageManifestPath(
    const std::string& storeDir,
    const std::string& imageId);

} // namespace paths {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_APPC_PATHS_HPP__
