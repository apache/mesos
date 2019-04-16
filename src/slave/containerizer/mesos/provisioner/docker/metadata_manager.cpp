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

#include <string>
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

#include "slave/state.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/message.hpp"
#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"

namespace spec = docker::spec;

using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class MetadataManagerProcess : public process::Process<MetadataManagerProcess>
{
public:
  MetadataManagerProcess(const Flags& _flags) : flags(_flags) {}

  ~MetadataManagerProcess() override {}

  Future<Nothing> recover();

  Future<Image> put(const Image& image);

  Future<Option<Image>> get(
      const spec::ImageReference& reference,
      bool cached);

  Future<hashset<string>> prune(
      const vector<spec::ImageReference>& excludedImages);

private:
  // Write out metadata manager state to persistent store.
  Try<Nothing> persist();

  const Flags flags;

  // This is a lookup table for images that are stored in memory. It is keyed
  // by image name.
  // For example, "ubuntu:14.04" -> ubuntu14:04 Image.
  hashmap<string, Image> storedImages;
};


Try<Owned<MetadataManager>> MetadataManager::create(const Flags& flags)
{
  Owned<MetadataManagerProcess> process(new MetadataManagerProcess(flags));

  return Owned<MetadataManager>(new MetadataManager(process));
}


MetadataManager::MetadataManager(Owned<MetadataManagerProcess> process)
  : process(process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


MetadataManager::~MetadataManager()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> MetadataManager::recover()
{
  return dispatch(process.get(), &MetadataManagerProcess::recover);
}


Future<Image> MetadataManager::put(const Image& image)
{
  return dispatch(
      process.get(),
      &MetadataManagerProcess::put,
      image);
}


Future<Option<Image>> MetadataManager::get(
    const spec::ImageReference& reference,
    bool cached)
{
  return dispatch(
      process.get(),
      &MetadataManagerProcess::get,
      reference,
      cached);
}


Future<hashset<string>> MetadataManager::prune(
    const vector<spec::ImageReference>& excludedImages)
{
  return dispatch(
      process.get(),
      &MetadataManagerProcess::prune,
      excludedImages);
}


Future<Image> MetadataManagerProcess::put(const Image& image)
{
  const string imageReference = stringify(image.reference());
  storedImages[imageReference] = image;

  Try<Nothing> status = persist();
  if (status.isError()) {
    return Failure("Failed to save state of Docker images: " + status.error());
  }

  VLOG(1) << "Successfully cached image '" << imageReference << "'";

  return image;
}


Future<Option<Image>> MetadataManagerProcess::get(
    const spec::ImageReference& reference,
    bool cached)
{
  const string imageReference = stringify(reference);

  VLOG(1) << "Looking for image '" << imageReference << "'";

  if (!storedImages.contains(imageReference)) {
    return None();
  }

  if (!cached) {
    VLOG(1) << "Ignored cached image '" << imageReference << "'";
    return None();
  }

  return storedImages[imageReference];
}


Future<hashset<string>> MetadataManagerProcess::prune(
    const vector<spec::ImageReference>& excludedImages)
{
  hashmap<string, Image> retainedImages;
  hashset<string> retainedLayers;

  foreach (const spec::ImageReference& reference, excludedImages) {
    const string imageName = stringify(reference);
    Option<Image> image = storedImages.get(imageName);

    if (image.isNone()) {
      // This is possible if docker store was cleaned
      // in a recovery after the container using this image was
      // launched.
      VLOG(1) << "Excluded docker image '" << imageName
              << "' is not cached in metadata manager.";
      continue;
    }

    retainedImages[imageName] = image.get();

    foreach (const string& layerId, image->layer_ids()) {
      retainedLayers.insert(layerId);
    }

    if (image->has_config_digest()) {
      retainedLayers.insert(image->config_digest());
    }
  }

  storedImages = std::move(retainedImages);

  Try<Nothing> status = persist();
  if (status.isError()) {
    return Failure("Failed to save state of Docker images: " + status.error());
  }

  return retainedLayers;
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

  Result<Images> images = state::read<Images>(storedImagesPath);
  if (images.isError()) {
    return Failure("Failed to read images from '" + storedImagesPath + "' " +
                   images.error());
  }

  if (images.isNone()) {
    // This could happen if the slave is hard rebooted after the file is created
    // but before the data is synced on disk.
    LOG(WARNING) << "The images file '" << storedImagesPath << "' is empty";

    return Nothing();
  }

  foreach (const Image& image, images->images()) {
    const string imageReference = stringify(image.reference());

    if (storedImages.contains(imageReference)) {
      LOG(WARNING) << "Found duplicate image in recovery for image reference '"
                   << imageReference << "'";
    } else {
      storedImages[imageReference] = image;
    }

    VLOG(1) << "Successfully loaded image '" << imageReference << "'";
  }

  LOG(INFO) << "Successfully loaded " << storedImages.size()
            << " Docker images";

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
