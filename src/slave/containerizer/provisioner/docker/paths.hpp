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

#ifndef __PROVISIONER_DOCKER_PATHS_HPP__
#define __PROVISIONER_DOCKER_PATHS_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace paths {

/**
 * The Docker store file system layout is as follows:
 * Image store dir ('--docker_store_dir' slave flag)
 *    |--staging
 *       |-- <temp_dir_archive>
 *           |-- <layer_id>
 *               |-- rootfs
 *    |--layers
 *       |--<layer_id>
 *           |--rootfs
 *    |--storedImages (file holding on cached images)
 */

std::string getStagingDir(const std::string& storeDir);


std::string getStagingTempDir(const std::string& storeDir);


std::string getImageArchiveTarPath(
    const std::string& discoveryDir,
    const std::string& name);


std::string getImageArchiveRepositoriesPath(const std::string& archivePath);


std::string getImageArchiveLayerPath(
    const std::string& archivePath,
    const std::string& layerId);


std::string getImageArchiveLayerManifestPath(
    const std::string& archivePath,
    const std::string& layerId);


std::string getImageArchiveLayerTarPath(
  const std::string& archivePath,
  const std::string& layerId);


std::string getImageArchiveLayerRootfsPath(
  const std::string& archivePath,
  const std::string& layerId);


std::string getImageLayerPath(
    const std::string& storeDir,
    const std::string& layerId);


std::string getImageLayerRootfsPath(
    const std::string& storeDir,
    const std::string& layerId);


std::string getStoredImagesPath(const std::string& storeDir);

} // namespace paths {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_PATHS_HPP__
