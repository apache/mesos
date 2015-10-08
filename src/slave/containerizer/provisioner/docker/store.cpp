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

#include "slave/containerizer/provisioner/docker/store.hpp"

#include <list>
#include <vector>

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/provisioner/docker/paths.hpp"
#include "slave/containerizer/provisioner/docker/puller.hpp"

#include "slave/flags.hpp"

using namespace process;

using std::list;
using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(
      const Flags& _flags,
      const Owned<MetadataManager>& _metadataManager,
      const Owned<Puller>& _puller)
    : flags(_flags), metadataManager(_metadataManager), puller(_puller) {}

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<vector<string>> get(const mesos::Image& image);

private:
  Future<Image> _get(
      const Image::Name& name,
      const Option<Image>& image);

  Future<vector<string>> __get(const Image& image);

  Future<vector<string>> moveLayers(
      const std::string& staging,
      const std::list<pair<string, string>>& layerPaths);

  Future<Image> storeImage(
      const Image::Name& name,
      const std::vector<std::string>& layerIds);

  Future<Nothing> moveLayer(const pair<string, string>& layerPath);

  const Flags flags;
  Owned<MetadataManager> metadataManager;
  Owned<Puller> puller;
};


Try<Owned<slave::Store>> Store::create(const Flags& flags)
{
  Try<Owned<Puller>> puller = Puller::create(flags);
  if (puller.isError()) {
    return Error("Failed to create Docker puller: " + puller.error());
  }

  if (!os::exists(flags.docker_store_dir)) {
    Try<Nothing> mkdir = os::mkdir(flags.docker_store_dir);
    if (mkdir.isError()) {
      return Error("Failed to create Docker store directory: " + mkdir.error());
    }
  }

  if (!os::exists(paths::getStagingDir(flags.docker_store_dir))) {
    Try<Nothing> mkdir =
      os::mkdir(paths::getStagingDir(flags.docker_store_dir));

    if (mkdir.isError()) {
      return Error("Failed to create Docker store staging directory: " +
                   mkdir.error());
    }
  }

  Try<Owned<MetadataManager>> metadataManager = MetadataManager::create(flags);
  if (metadataManager.isError()) {
    return Error(metadataManager.error());
  }

  Owned<StoreProcess> process(
      new StoreProcess(flags, metadataManager.get(), puller.get()));

  return Owned<slave::Store>(new Store(process));
}


Store::Store(const Owned<StoreProcess>& _process) : process(_process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


Store::~Store()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> Store::recover()
{
  return dispatch(process.get(), &StoreProcess::recover);
}


Future<vector<string>> Store::get(const mesos::Image& image)
{
  return dispatch(process.get(), &StoreProcess::get, image);
}


Future<vector<string>> StoreProcess::get(const mesos::Image& image)
{
  if (image.type() != mesos::Image::DOCKER) {
    return Failure("Docker provisioner store only supports Docker images");
  }

  Try<Image::Name> imageName = parseName(image.docker().name());
  if (imageName.isError()) {
    return Failure("Unable to parse docker image name: " + imageName.error());
  }

  return metadataManager->get(imageName.get())
    .then(defer(self(), &Self::_get, imageName.get(), lambda::_1))
    .then(defer(self(), &Self::__get, lambda::_1));
}


Future<Image> StoreProcess::_get(
    const Image::Name& name,
    const Option<Image>& image)
{
  if (image.isSome()) {
    return image.get();
  }

  Try<string> staging =
    os::mkdtemp(paths::getStagingTempDir(flags.docker_store_dir));

  if (staging.isError()) {
    return Failure("Failed to create a staging directory");
  }

  return puller->pull(name, staging.get())
    .then(defer(self(), &Self::moveLayers, staging.get(), lambda::_1))
    .then(defer(self(), &Self::storeImage, name, lambda::_1))
    .onAny([staging]() {
      Try<Nothing> rmdir = os::rmdir(staging.get());
      if (rmdir.isError()) {
        LOG(WARNING) << "Failed to remove staging directory: " << rmdir.error();
      }
    });
}


Future<vector<string>> StoreProcess::__get(const Image& image)
{
  vector<string> layerDirectories;
  foreach (const string& layer, image.layer_ids()) {
    layerDirectories.push_back(
        paths::getImageLayerRootfsPath(
            flags.docker_store_dir, layer));
  }

  return layerDirectories;
}


Future<Nothing> StoreProcess::recover()
{
  return metadataManager->recover();
}


Future<vector<string>> StoreProcess::moveLayers(
    const string& staging,
    const list<pair<string, string>>& layerPaths)
{
  list<Future<Nothing>> futures;
  foreach (const auto& layerPath, layerPaths) {
    futures.push_back(moveLayer(layerPath));
  }

  return collect(futures)
    .then([layerPaths]() {
        vector<string> layerIds;
        foreach (const auto& layerPath, layerPaths) {
          layerIds.push_back(layerPath.first);
        }

        return layerIds;
    });
}


Future<Image> StoreProcess::storeImage(
    const Image::Name& name,
    const vector<string>& layerIds)
{
  return metadataManager->put(name, layerIds);
}


Future<Nothing> StoreProcess::moveLayer(const pair<string, string>& layerPath)
{
  if (!os::exists(layerPath.second)) {
    return Failure("Unable to find layer '" + layerPath.first + "' in '" +
                   layerPath.second + "'");
  }

  const string imageLayerPath =
    paths::getImageLayerPath(flags.docker_store_dir, layerPath.first);

  if (!os::exists(imageLayerPath)) {
    Try<Nothing> mkdir = os::mkdir(imageLayerPath);
    if (mkdir.isError()) {
      return Failure("Failed to create layer path in store for id '" +
                     layerPath.first + "': " + mkdir.error());
    }
  }

  Try<Nothing> status = os::rename(
      layerPath.second,
      paths::getImageLayerRootfsPath(
          flags.docker_store_dir, layerPath.first));

  if (status.isError()) {
    return Failure("Failed to move layer '" + layerPath.first +
                   "' to store directory: " + status.error());
  }

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
