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

#include <vector>

#include <glog/logging.h>

#include <mesos/appc/spec.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#include "slave/containerizer/mesos/provisioner/appc/cache.hpp"
#include "slave/containerizer/mesos/provisioner/appc/fetcher.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

namespace spec = appc::spec;

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;

using process::defer;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(
      const string& rootDir,
      Owned<Cache> cache,
      Owned<Fetcher> fetcher);

  ~StoreProcess() override {}

  Future<Nothing> recover();

  Future<ImageInfo> get(const Image& image);

private:
  Future<vector<string>> fetchImage(
      const Image::Appc& appc,
      bool cached);

  Future<vector<string>> fetchDependencies(
      const string& imageId,
      bool cached);

  Future<string> _fetchImage(const Image::Appc& appc);

  Future<vector<string>> __fetchImage(
      const string& imageId,
      bool cached);

  // Absolute path to the root directory of the store as defined by
  // --appc_store_dir.
  const string rootDir;

  Owned<Cache> cache;
  Owned<Fetcher> fetcher;
};


Try<Owned<slave::Store>> Store::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
  Try<Nothing> mkdir = os::mkdir(paths::getImagesDir(flags.appc_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create the images directory: " + mkdir.error());
  }

  // Make sure the root path is canonical so all image paths derived
  // from it are canonical too.
  Result<string> rootDir = os::realpath(flags.appc_store_dir);
  if (!rootDir.isSome()) {
    return Error(
        "Failed to get the realpath of the store root directory: " +
        (rootDir.isError() ? rootDir.error() : "not found"));
  }

  Try<Owned<Cache>> cache = Cache::create(Path(rootDir.get()));
  if (cache.isError()) {
    return Error("Failed to create image cache: " + cache.error());
  }

  Try<Nothing> recover = cache.get()->recover();
  if (recover.isError()) {
    return Error("Failed to load image cache: " + recover.error());
  }

  // TODO(jojy): Uri fetcher has 'shared' semantics for the
  // provisioner. It's a shared pointer which needs to be injected
  // from top level into the store (instead of being created here).
  uri::fetcher::Flags _flags;
  _flags.curl_stall_timeout = flags.fetcher_stall_timeout;

  Try<Owned<uri::Fetcher>> uriFetcher = uri::fetcher::create(_flags);
  if (uriFetcher.isError()) {
    return Error("Failed to create uri fetcher: " + uriFetcher.error());
  }

  Try<Owned<Fetcher>> fetcher = Fetcher::create(flags, uriFetcher->share());
  if (fetcher.isError()) {
    return Error("Failed to create image fetcher: " + fetcher.error());
  }

  return Owned<slave::Store>(new Store(Owned<StoreProcess>(new StoreProcess(
      rootDir.get(),
      cache.get(),
      fetcher.get()))));
}


Store::Store(Owned<StoreProcess> _process)
  : process(_process)
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


Future<ImageInfo> Store::get(const Image& image, const string& backend)
{
  return dispatch(process.get(), &StoreProcess::get, image);
}


StoreProcess::StoreProcess(
    const string& _rootDir,
    Owned<Cache> _cache,
    Owned<Fetcher> _fetcher)
  : ProcessBase(process::ID::generate("appc-provisioner-store")),
    rootDir(_rootDir),
    cache(_cache),
    fetcher(_fetcher) {}


Future<Nothing> StoreProcess::recover()
{
  Try<Nothing> recover = cache->recover();
  if (recover.isError()) {
    return Failure("Failed to recover cache: " + recover.error());
  }

  return Nothing();
}


Future<ImageInfo> StoreProcess::get(const Image& image)
{
  if (image.type() != Image::APPC) {
    return Failure("Not an Appc image: " + stringify(image.type()));
  }

  const Image::Appc& appc = image.appc();

  const Path stagingDir(paths::getStagingDir(rootDir));

  Try<Nothing> staging = os::mkdir(stagingDir);
  if (staging.isError()) {
    return Failure("Failed to create staging directory: " + staging.error());
  }

  return fetchImage(appc, image.cached())
    .then(defer(self(), [=](const vector<string>& imageIds)
          -> Future<ImageInfo> {
      // Appc image contains the manifest at the top layer and the
      // image id is at index 0.
      Try<spec::ImageManifest> manifest = spec::getManifest(
          paths::getImagePath(rootDir, imageIds.at(0)));

      if (manifest.isError()) {
        return Failure(
            "Failed to get manifest for Appc image '" +
            appc.SerializeAsString() + "': " + manifest.error());
      }

      vector<string> rootfses;
      // TODO(jojy): Print a warning if there are duplicated image ids
      // in the list. The semantics is weird when there are duplicated
      // image ids in the list. Appc spec does not discuss this
      // situation.
      foreach (const string& imageId, imageIds) {
        rootfses.emplace_back(paths::getImageRootfsPath(rootDir, imageId));
      }

      return ImageInfo{rootfses, None(), manifest.get()};
    }));
}


// Fetches the image into the 'staging' directory, and recursively
// fetches the image's dependencies in a depth first order.
Future<vector<string>> StoreProcess::fetchImage(
    const Image::Appc& appc,
    bool cached)
{
  Option<string> imageId = appc.has_id() ? appc.id() : cache->find(appc);
  if (cached && imageId.isSome()) {
    if (os::exists(paths::getImagePath(rootDir, imageId.get()))) {
      VLOG(1) << "Image '" << appc.name() << "' is found in cache with "
              << "image id '" << imageId.get() << "'";

      return __fetchImage(imageId.get(), cached);
    }
  }

  return _fetchImage(appc)
    .then(defer(self(), &Self::__fetchImage, lambda::_1, cached));
}


Future<string> StoreProcess::_fetchImage(const Image::Appc& appc)
{
  VLOG(1) << "Fetching image '" << appc.name() << "'";

  Try<string> _tmpFetchDir = os::mkdtemp(
      path::join(paths::getStagingDir(rootDir), "XXXXXX"));

  if (_tmpFetchDir.isError()) {
    return Failure(
        "Failed to create temporary fetch directory for image '" +
        appc.name() + "': " + _tmpFetchDir.error());
  }

  const string tmpFetchDir = _tmpFetchDir.get();

  return fetcher->fetch(appc, Path(tmpFetchDir))
    .then(defer(self(), [=]() -> Future<string> {
      Try<list<string>> imageIds = os::ls(tmpFetchDir);
      if (imageIds.isError()) {
        return Failure(
            "Failed to list images under '" + tmpFetchDir +
            "': " + imageIds.error());
      }

      if (imageIds->size() != 1) {
        return Failure(
            "Unexpected number of images under '" + tmpFetchDir +
            "': " + stringify(imageIds->size()));
      }

      const string& imageId = imageIds->front();
      const string source = path::join(tmpFetchDir, imageId);
      const string target = paths::getImagePath(rootDir, imageId);

      if (os::exists(target)) {
        LOG(WARNING) << "Image id '" << imageId
                     << "' already exists in the store";
      } else {
        Try<Nothing> rename = os::rename(source, target);
        if (rename.isError()) {
          return Failure(
              "Failed to rename directory '" + source +
              "' to '" + target + "': " + rename.error());
        }
      }

      Try<Nothing> addCache = cache->add(imageId);
      if (addCache.isError()) {
        return Failure(
            "Failed to add image '" + appc.name() + "' with image id '" +
            imageId + "' to the cache: " + addCache.error());
      }

      Try<Nothing> rmdir = os::rmdir(tmpFetchDir);
      if (rmdir.isError()) {
        return Failure(
            "Failed to remove temporary fetch directory '" +
            tmpFetchDir + "' for image '" + appc.name() + "': " +
            rmdir.error());
      }

      return imageId;
    }));
}


Future<vector<string>> StoreProcess::__fetchImage(
    const string& imageId,
    bool cached)
{
  return fetchDependencies(imageId, cached)
    .then([imageId](vector<string> imageIds) -> vector<string> {
      imageIds.emplace_back(imageId);

      return imageIds;
    });
}


Future<vector<string>> StoreProcess::fetchDependencies(
    const string& imageId,
    bool cached)
{
  const string imagePath = paths::getImagePath(rootDir, imageId);

  Try<spec::ImageManifest> manifest = spec::getManifest(imagePath);
  if (manifest.isError()) {
    return Failure(
        "Failed to get dependencies for image id '" + imageId +
        "': " + manifest.error());
  }

  vector<Image::Appc> dependencies;
  foreach (const spec::ImageManifest::Dependency& dependency,
           manifest->dependencies()) {
    Image::Appc appc;
    appc.set_name(dependency.imagename());
    if (dependency.has_imageid()) {
      appc.set_id(dependency.imageid());
    }

    // TODO(jojy): Make Image::Appc use appc::spec::Label instead of
    // mesos::Label so that we can avoid this loop here.
    foreach (const spec::ImageManifest::Label& label, dependency.labels()) {
      mesos::Label appcLabel;
      appcLabel.set_key(label.name());
      appcLabel.set_value(label.value());

      appc.mutable_labels()->add_labels()->CopyFrom(appcLabel);
    }

    dependencies.emplace_back(appc);
  }

  if (dependencies.size() == 0) {
    return vector<string>();
  }

  // Do a depth first search.
  vector<Future<vector<string>>> futures;
  futures.reserve(dependencies.size());
  foreach (const Image::Appc& appc, dependencies) {
    futures.emplace_back(fetchImage(appc, cached));
  }

  return collect(futures)
    .then(defer(self(), [=](const vector<vector<string>>& imageIdsList) {
      vector<string> result;
      foreach (const vector<string>& imageIds, imageIdsList) {
        result.insert(result.end(), imageIds.begin(), imageIds.end());
      }

      return result;
    }));
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
