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

#include "slave/containerizer/provisioner/docker/metadata_manager.hpp"

#include <vector>

#include <glog/logging.h>

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/owned.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/provisioner/docker/paths.hpp"
#include "slave/containerizer/provisioner/docker/message.hpp"

#include "slave/state.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class MetadataManagerProcess : public process::Process<MetadataManagerProcess>
{
public:
  MetadataManagerProcess(const Flags& _flags) : flags(_flags) {}

  ~MetadataManagerProcess() {}

  Future<Nothing> recover();

  Future<Image> put(
      const Image::Name& name,
      const std::vector<std::string>& layerIds);

  Future<Option<Image>> get(const Image::Name& name);

  // TODO(chenlily): Implement removal of unreferenced images.

private:
  // Write out metadata manager state to persistent store.
  Try<Nothing> persist();

  const Flags flags;

  // This is a lookup table for images that are stored in memory. It is keyed
  // by the name of the Image.
  // For example, "ubuntu:14.04" -> ubuntu14:04 Image.
  hashmap<std::string, Image> storedImages;
};


Try<Owned<MetadataManager>> MetadataManager::create(const Flags& flags)
{
  Owned<MetadataManagerProcess> process(new MetadataManagerProcess(flags));

  return Owned<MetadataManager>(new MetadataManager(process));
}


MetadataManager::MetadataManager(Owned<MetadataManagerProcess> process)
  : process(process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


MetadataManager::~MetadataManager()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> MetadataManager::recover()
{
  return process::dispatch(process.get(), &MetadataManagerProcess::recover);
}


Future<Image> MetadataManager::put(
    const Image::Name& name,
    const vector<string>& layerIds)
{
  return dispatch(
      process.get(),
      &MetadataManagerProcess::put,
      name,
      layerIds);
}


Future<Option<Image>> MetadataManager::get(const Image::Name& name)
{
  return dispatch(process.get(), &MetadataManagerProcess::get, name);
}


Future<Image> MetadataManagerProcess::put(
    const Image::Name& name,
    const vector<string>& layerIds)
{
  const string imageName = stringify(name);

  Image dockerImage;
  dockerImage.mutable_name()->CopyFrom(name);
  foreach (const string& layerId, layerIds) {
    dockerImage.add_layer_ids(layerId);
  }

  storedImages[imageName] = dockerImage;

  Try<Nothing> status = persist();
  if (status.isError()) {
    return Failure("Failed to save state of Docker images: " + status.error());
  }

  return dockerImage;
}


Future<Option<Image>> MetadataManagerProcess::get(
    const Image::Name& name)
{
  const string imageName = stringify(name);

  if (!storedImages.contains(imageName)) {
    return None();
  }

  return storedImages[imageName];
}


Try<Nothing> MetadataManagerProcess::persist()
{
  Images images;

  foreachvalue (const Image& image, storedImages) {
    images.add_images()->CopyFrom(image);
  }

  Try<Nothing> status = state::checkpoint(
      paths::getStoredImagesPath(flags.docker_store_dir), images);
  if (status.isError()) {
    return Error("Failed to perform checkpoint: " + status.error());
  }

  return Nothing();
}


Future<Nothing> MetadataManagerProcess::recover()
{
  string storedImagesPath = paths::getStoredImagesPath(flags.docker_store_dir);

  if (!os::exists(storedImagesPath)) {
    LOG(INFO) << "No images to load from disk. Docker provisioner image "
              << "storage path '" << storedImagesPath << "' does not exist";
    return Nothing();
  }

  Result<Images> images = ::protobuf::read<Images>(storedImagesPath);
  if (images.isError()) {
    return Failure("Failed to read protobuf for Docker provisioner image: " +
                   images.error());
  }

  foreach (const Image image, images.get().images()) {
    vector<string> missingLayerIds;
    foreach (const string layerId, image.layer_ids()) {
      const string rootfsPath =
        paths::getImageLayerRootfsPath(flags.docker_store_dir, layerId);

      if (!os::exists(rootfsPath)) {
        missingLayerIds.push_back(layerId);
      }
    }

    if (!missingLayerIds.empty()) {
      LOG(WARNING) << "Skipped loading image  '" << stringify(image.name())
                   << "' due to missing layers: " << stringify(missingLayerIds);
      continue;
    }

    const string imageName = stringify(image.name());
    if (storedImages.contains(imageName)) {
      LOG(WARNING) << "Found duplicate image in recovery for image name '"
                   << imageName << "'";
    } else {
      storedImages[imageName] = image;
    }
  }

  LOG(INFO) << "Loaded " << storedImages.size() << " Docker images";

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
