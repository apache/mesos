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

#include "messages/docker_provisioner.hpp"

#include "slave/containerizer/provisioners/docker/paths.hpp"
#include "slave/containerizer/provisioners/docker/metadata_manager.hpp"
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
  ~MetadataManagerProcess() {}

  static Try<process::Owned<MetadataManagerProcess>> create(
      const Flags& flags);

  Future<DockerImage> put(
      const ImageName& name,
      const std::list<std::string>& layerIds);

  Future<Option<DockerImage>> get(const ImageName& name);

  Future<Nothing> recover();

  // TODO(chenlily): Implement removal of unreferenced images.

private:
  MetadataManagerProcess(const Flags& flags);

  // Write out metadata manager state to persistent store.
  Try<Nothing> persist();

  const Flags flags;

  // This is a lookup table for images that are stored in memory. It is keyed
  // by the name of the DockerImage.
  // For example, "ubuntu:14.04" -> ubuntu14:04 DockerImage.
  hashmap<std::string, DockerImage> storedImages;
};


Try<Owned<MetadataManager>> MetadataManager::create(const Flags& flags)
{
  Try<Owned<MetadataManagerProcess>> process =
    MetadataManagerProcess::create(flags);
  if (process.isError()) {
    return Error("Failed to create Metadata Manager: " + process.error());
  }
  return Owned<MetadataManager>(new MetadataManager(process.get()));
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


Future<DockerImage> MetadataManager::put(
    const ImageName& name,
    const list<string>& layerIds)
{
  return dispatch(
      process.get(), &MetadataManagerProcess::put, name, layerIds);
}


Future<Option<DockerImage>> MetadataManager::get(const ImageName& name)
{
  return dispatch(process.get(), &MetadataManagerProcess::get, name);
}


MetadataManagerProcess::MetadataManagerProcess(const Flags& flags)
  : flags(flags) {}


Try<Owned<MetadataManagerProcess>> MetadataManagerProcess::create(
    const Flags& flags)
{
  Owned<MetadataManagerProcess> metadataManager =
    Owned<MetadataManagerProcess>(new MetadataManagerProcess(flags));

  return metadataManager;
}


Future<DockerImage> MetadataManagerProcess::put(
    const ImageName& name,
    const list<string>& layerIds)
{
  storedImages[name.name()] = DockerImage(name, layerIds);

  Try<Nothing> status = persist();
  if (status.isError()) {
    return Failure("Failed to save state of Docker images" + status.error());
  }

  return storedImages[name.name()];
}


Future<Option<DockerImage>> MetadataManagerProcess::get(const ImageName& name)
{
  if (!storedImages.contains(name.name())) {
    return None();
  }

  return storedImages[name.name()];
}


Try<Nothing> MetadataManagerProcess::persist()
{
  DockerProvisionerImages images;

  foreachpair(
      const string& name, const DockerImage& dockerImage, storedImages) {
    DockerProvisionerImages::Image* image = images.add_images();

    image->set_name(name);

    foreach (const string& layerId, dockerImage.layerIds) {
      image->add_layer_ids(layerId);
    }
  }

  Try<Nothing> status = mesos::internal::slave::state::checkpoint(
      paths::getStoredImagesPath(flags.docker_store_dir), images);
  if (status.isError()) {
    return Error("Failed to perform checkpoint: " + status.error());
  }

  return Nothing();
}


Future<Nothing> MetadataManagerProcess::recover()
{
  string storedImagesPath = paths::getStoredImagesPath(flags.docker_store_dir);

  storedImages.clear();
  if (!os::exists(storedImagesPath)) {
    LOG(INFO) << "No images to load from disk. Docker provisioner image "
              << "storage path: " << storedImagesPath << " does not exist.";
    return Nothing();
  }

  Result<DockerProvisionerImages> images =
    ::protobuf::read<DockerProvisionerImages>(storedImagesPath);
  if (images.isError()) {
    return Failure("Failed to read protobuf for Docker provisioner image: " +
                   images.error());
  }

  for (int i = 0; i < images.get().images_size(); i++) {
    string name = images.get().images(i).name();

    list<string> layerIds;
    vector<string> missingLayerIds;
    for (int j = 0; j < images.get().images(i).layer_ids_size(); j++) {
      string layerId = images.get().images(i).layer_ids(j);

      layerIds.push_back(layerId);

      if (!os::exists(
              paths::getImageLayerRootfsPath(flags.docker_store_dir, layerId))) {
        missingLayerIds.push_back(layerId);
      }
    }

    if (!missingLayerIds.empty()) {
      foreach (const string& layerId, missingLayerIds) {
        LOG(WARNING) << "Image layer: " << layerId << " required for Docker "
                     << "image: " << name << " is not on disk.";
      }
      LOG(WARNING) << "Skipped loading image: " << name
                   << " due to missing layers.";
      continue;
    }

    Try<ImageName> imageName = ImageName::create(name);
    if (imageName.isError()) {
      return Failure("Unable to parse Docker image name: " + imageName.error());
    }
    storedImages[imageName.get().name()] = DockerImage(imageName.get(), layerIds);
  }

  LOG(INFO) << "Loaded " << storedImages.size() << " Docker images.";

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
