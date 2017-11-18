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

#include "slave/containerizer/mesos/provisioner/constants.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"

#include <process/clock.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

using std::list;
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


string getImageLayerPath(const string& storeDir, const string& layerId)
{
  return path::join(storeDir, "layers", layerId);
}


Try<list<string>> listLayers(const string& storeDir)
{
  const string layersDir = path::join(storeDir, "layers");
  return os::ls(layersDir);
}


string getImageLayerManifestPath(const string& layerPath)
{
  return path::join(layerPath, "json");
}


string getImageLayerManifestPath(const string& storeDir, const string& layerId)
{
  return getImageLayerManifestPath(getImageLayerPath(storeDir, layerId));
}


string getImageLayerRootfsPath(const string& layerPath, const string& backend)
{
  if (backend == OVERLAY_BACKEND) {
    return path::join(layerPath, "rootfs." + backend);
  }

  return path::join(layerPath, "rootfs");
}


string getImageLayerRootfsPath(
    const string& storeDir,
    const string& layerId,
    const string& backend)
{
  return getImageLayerRootfsPath(getImageLayerPath(storeDir, layerId), backend);
}


string getImageLayerTarPath(const string& layerPath)
{
  return path::join(layerPath, "layer.tar");
}


string getImageLayerTarPath(const string& storeDir, const string& layerId)
{
  return getImageLayerTarPath(getImageLayerPath(storeDir, layerId));
}


string getImageArchiveTarPath(const string& discoveryDir, const string& name)
{
  return path::join(discoveryDir, name + ".tar");
}


string getStoredImagesPath(const string& storeDir)
{
  return path::join(storeDir, "storedImages");
}


string getGcDir(const string& storeDir)
{
  return path::join(storeDir, "gc");
}


string getGcLayerPath(const string& storeDir, const string& layerId)
{
  return path::join(
      getGcDir(storeDir),
      layerId + "." + stringify(process::Clock::now().duration().ns()));
}

} // namespace paths {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
