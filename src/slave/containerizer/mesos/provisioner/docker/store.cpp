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

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <mesos/docker/spec.hpp>

#include "slave/containerizer/mesos/provisioner/constants.hpp"
#include "slave/containerizer/mesos/provisioner/utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

#include "uri/fetcher.hpp"

using namespace process;

namespace spec = docker::spec;

using std::list;
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
    : ProcessBase(process::ID::generate("docker-provisioner-store")),
      flags(_flags),
      metadataManager(_metadataManager),
      puller(_puller) {}

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<ImageInfo> get(
      const mesos::Image& image,
      const string& backend);

private:
  Future<Image> _get(
      const spec::ImageReference& reference,
      const Option<Image>& image,
      const string& backend);

  Future<ImageInfo> __get(
      const Image& image,
      const string& backend);

  Future<vector<string>> moveLayers(
      const string& staging,
      const vector<string>& layerIds,
      const string& backend);

  Future<Nothing> moveLayer(
      const string& staging,
      const string& layerId,
      const string& backend);

  const Flags flags;

  Owned<MetadataManager> metadataManager;
  Owned<Puller> puller;
  hashmap<string, Owned<Promise<Image>>> pulling;
};


Try<Owned<slave::Store>> Store::create(const Flags& flags)
{
  // TODO(jieyu): We should inject URI fetcher from top level, instead
  // of creating it here.
  uri::fetcher::Flags _flags;

  // TODO(dpravat): Remove after resolving MESOS-5473.
#ifndef __WINDOWS__
  _flags.docker_config = flags.docker_config;
#endif

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(_flags);
  if (fetcher.isError()) {
    return Error("Failed to create the URI fetcher: " + fetcher.error());
  }

  Try<Owned<Puller>> puller = Puller::create(flags, fetcher->share());
  if (puller.isError()) {
    return Error("Failed to create Docker puller: " + puller.error());
  }

  Try<Owned<slave::Store>> store = Store::create(flags, puller.get());
  if (store.isError()) {
    return Error("Failed to create Docker store: " + store.error());
  }

  return store.get();
}


Try<Owned<slave::Store>> Store::create(
    const Flags& flags,
    const Owned<Puller>& puller)
{
  Try<Nothing> mkdir = os::mkdir(flags.docker_store_dir);
  if (mkdir.isError()) {
    return Error("Failed to create Docker store directory: " +
                 mkdir.error());
  }

  mkdir = os::mkdir(paths::getStagingDir(flags.docker_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create Docker store staging directory: " +
                 mkdir.error());
  }

  Try<Owned<MetadataManager>> metadataManager = MetadataManager::create(flags);
  if (metadataManager.isError()) {
    return Error(metadataManager.error());
  }

  Owned<StoreProcess> process(
      new StoreProcess(flags, metadataManager.get(), puller));

  return Owned<slave::Store>(new Store(process));
}


Store::Store(Owned<StoreProcess> _process) : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Store::~Store()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> Store::recover()
{
  return dispatch(process.get(), &StoreProcess::recover);
}


Future<ImageInfo> Store::get(
    const mesos::Image& image,
    const string& backend)
{
  return dispatch(process.get(), &StoreProcess::get, image, backend);
}


Future<Nothing> StoreProcess::recover()
{
  return metadataManager->recover();
}


Future<ImageInfo> StoreProcess::get(
    const mesos::Image& image,
    const string& backend)
{
  if (image.type() != mesos::Image::DOCKER) {
    return Failure("Docker provisioner store only supports Docker images");
  }

  Try<spec::ImageReference> reference =
    spec::parseImageReference(image.docker().name());

  if (reference.isError()) {
    return Failure("Failed to parse docker image '" + image.docker().name() +
                   "': " + reference.error());
  }

  return metadataManager->get(reference.get(), image.cached())
    .then(defer(self(), &Self::_get, reference.get(), lambda::_1, backend))
    .then(defer(self(), &Self::__get, lambda::_1, backend));
}


Future<Image> StoreProcess::_get(
    const spec::ImageReference& reference,
    const Option<Image>& image,
    const string& backend)
{
  // NOTE: Here, we assume that image layers are not removed without
  // first removing the metadata in the metadata manager first.
  // Otherwise, the image we return here might miss some layers. At
  // the time we introduce cache eviction, we also want to avoid the
  // situation where a layer was returned to the provisioner but is
  // later evicted.
  if (image.isSome()) {
    // It is possible that a layer is missed after recovery if the
    // agent flag `--image_provisioner_backend` is changed from a
    // specified backend to `None()`. We need to check that each
    // layer exists for a cached image.
    bool layerMissed = false;

    foreach (const string& layerId, image.get().layer_ids()) {
      const string rootfsPath = paths::getImageLayerRootfsPath(
          flags.docker_store_dir,
          layerId,
          backend);

      if (!os::exists(rootfsPath)) {
        layerMissed = true;
        break;
      }
    }

    if (!layerMissed) {
      return image.get();
    }
  }

  Try<string> staging =
    os::mkdtemp(paths::getStagingTempDir(flags.docker_store_dir));

  if (staging.isError()) {
    return Failure("Failed to create a staging directory: " + staging.error());
  }

  // If there is already an pulling going on for the given 'name', we
  // will skip the additional pulling.
  const string name = stringify(reference);

  if (!pulling.contains(name)) {
    Owned<Promise<Image>> promise(new Promise<Image>());

    Future<Image> future = puller->pull(reference, staging.get(), backend)
      .then(defer(self(),
                  &Self::moveLayers,
                  staging.get(),
                  lambda::_1,
                  backend))
      .then(defer(self(), [=](const vector<string>& layerIds) {
        return metadataManager->put(reference, layerIds);
      }))
      .onAny(defer(self(), [=](const Future<Image>&) {
        pulling.erase(name);

        Try<Nothing> rmdir = os::rmdir(staging.get());
        if (rmdir.isError()) {
          LOG(WARNING) << "Failed to remove staging directory: "
                       << rmdir.error();
        }
      }));

    promise->associate(future);
    pulling[name] = promise;

    return promise->future();
  }

  return pulling[name]->future();
}


Future<ImageInfo> StoreProcess::__get(
    const Image& image,
    const string& backend)
{
  CHECK_LT(0, image.layer_ids_size());

  vector<string> layerPaths;
  foreach (const string& layerId, image.layer_ids()) {
    layerPaths.push_back(paths::getImageLayerRootfsPath(
        flags.docker_store_dir,
        layerId,
        backend));
  }

  // Read the manifest from the last layer because all runtime config
  // are merged at the leaf already.
  Try<string> manifest = os::read(
      paths::getImageLayerManifestPath(
          flags.docker_store_dir,
          image.layer_ids(image.layer_ids_size() - 1)));

  if (manifest.isError()) {
    return Failure("Failed to read manifest: " + manifest.error());
  }

  Try<::docker::spec::v1::ImageManifest> v1 =
    ::docker::spec::v1::parse(manifest.get());

  if (v1.isError()) {
    return Failure("Failed to parse docker v1 manifest: " + v1.error());
  }

  return ImageInfo{layerPaths, v1.get()};
}


Future<vector<string>> StoreProcess::moveLayers(
    const string& staging,
    const vector<string>& layerIds,
    const string& backend)
{
  list<Future<Nothing>> futures;
  foreach (const string& layerId, layerIds) {
    futures.push_back(moveLayer(staging, layerId, backend));
  }

  return collect(futures)
    .then([layerIds]() -> vector<string> { return layerIds; });
}


Future<Nothing> StoreProcess::moveLayer(
    const string& staging,
    const string& layerId,
    const string& backend)
{
  const string source = path::join(staging, layerId);

  // This is the case where the puller skips the pulling of the layer
  // because the layer already exists in the store.
  //
  // TODO(jieyu): Verify that the layer is actually in the store.
  if (!os::exists(source)) {
    return Nothing();
  }

  const string targetRootfs = paths::getImageLayerRootfsPath(
      flags.docker_store_dir,
      layerId,
      backend);

  // NOTE: Since the layer id is supposed to be unique. If the layer
  // already exists in the store, we'll skip the moving since they are
  // expected to be the same.
  if (os::exists(targetRootfs)) {
    return Nothing();
  }

  const string sourceRootfs = paths::getImageLayerRootfsPath(source, backend);
  const string target = paths::getImageLayerPath(
      flags.docker_store_dir,
      layerId);

#ifdef __linux__
  // If the backend is "overlay", we need to convert
  // AUFS whiteout files to OverlayFS whiteout files.
  if (backend == OVERLAY_BACKEND) {
    Try<Nothing> convert = convertWhiteouts(sourceRootfs);
    if (convert.isError()) {
      return Failure(
          "Failed to convert the whiteout files under '" +
          sourceRootfs + "': " + convert.error());
    }
  }
#endif

  if (!os::exists(target)) {
    // This is the case that we pull the layer for the first time.
    Try<Nothing> mkdir = os::mkdir(target);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create directory in store for layer '" +
          layerId + "': " + mkdir.error());
    }

    Try<Nothing> rename = os::rename(source, target);
    if (rename.isError()) {
      return Failure(
          "Failed to move layer from '" + source +
          "' to '" + target + "': " + rename.error());
    }
  } else {
    // This is the case where the layer has already been pulled with a
    // different backend.
    Try<Nothing> rename = os::rename(sourceRootfs, targetRootfs);
    if (rename.isError()) {
      return Failure(
          "Failed to move rootfs from '" + sourceRootfs +
          "' to '" + targetRootfs + "': " + rename.error());
    }
  }

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
