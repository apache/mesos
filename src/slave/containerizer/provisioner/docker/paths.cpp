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

#include "slave/containerizer/provisioner/docker/paths.hpp"

#include <stout/path.hpp>

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace paths {

string getStagingDir(const string& storeDir)
{
  return path::join(storeDir, "staging");
}


string getStagingTempDir(const string& storeDir)
{
  return path::join(getStagingDir(storeDir), "XXXXXX");
}


string getImageArchiveTarPath(
    const string& discoveryDir,
    const string& name)
{
  return path::join(discoveryDir, name + ".tar");
}


string getImageArchiveRepositoriesPath(const string& archivePath)
{
  return path::join(archivePath, "repositories");
}


std::string getImageArchiveLayerPath(
    const string& archivePath,
    const string& layerId)
{
  return path::join(archivePath, layerId);
}


string getImageArchiveLayerManifestPath(
    const string& archivePath,
    const string& layerId)
{
  return path::join(getImageArchiveLayerPath(archivePath, layerId), "json");
}


string getImageArchiveLayerTarPath(
  const string& archivePath,
  const string& layerId)
{
  return path::join(
      getImageArchiveLayerPath(archivePath, layerId), "layer.tar");
}


string getImageArchiveLayerRootfsPath(
    const string& archivePath,
    const string& layerId)
{
  return path::join(getImageArchiveLayerPath(archivePath, layerId), "rootfs");
}


string getImageLayerPath(
    const string& storeDir,
    const string& layerId)
{
  return path::join(storeDir, "layers", layerId);
}


string getImageLayerRootfsPath(
    const string& storeDir,
    const string& layerId)
{
  return path::join(getImageLayerPath(storeDir, layerId), "rootfs");
}


string getStoredImagesPath(const string& storeDir)
{
  return path::join(storeDir, "storedImages");
}

} // namespace paths {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
