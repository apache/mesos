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

#include <list>

#include <glog/logging.h>

#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <mesos/appc/spec.hpp>

#include "slave/containerizer/mesos/provisioner/appc/cache.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

using namespace process;

namespace spec = appc::spec;

using std::list;
using std::string;
using std::vector;

using process::Owned;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(const string& rootDir, Owned<Cache> cache);

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<ImageInfo> get(const Image& image);

private:
  // Absolute path to the root directory of the store as defined by
  // --appc_store_dir.
  const string rootDir;

  Owned<Cache> cache;
};


Try<Owned<slave::Store>> Store::create(const Flags& flags)
{
  Try<Nothing> mkdir = os::mkdir(paths::getImagesDir(flags.appc_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create the images directory: " + mkdir.error());
  }

  // Make sure the root path is canonical so all image paths derived
  // from it are canonical too.
  Result<string> rootDir = os::realpath(flags.appc_store_dir);
  if (!rootDir.isSome()) {
    // The above mkdir call recursively creates the store directory
    // if necessary so it cannot be None here.
    CHECK_ERROR(rootDir);

    return Error(
        "Failed to get the realpath of the store root directory: " +
        rootDir.error());
  }

  Try<Owned<Cache>> cache = Cache::create(Path(rootDir.get()));
  if (cache.isError()) {
    return Error("Failed to create image cache: " + cache.error());
  }

  return Owned<slave::Store>(new Store(
      Owned<StoreProcess>(new StoreProcess(rootDir.get(), cache.get()))));
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


Future<ImageInfo> Store::get(const Image& image)
{
  return dispatch(process.get(), &StoreProcess::get, image);
}


StoreProcess::StoreProcess(const string& _rootDir, Owned<Cache> _cache)
  : rootDir(_rootDir),
    cache(_cache) {}


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

  Option<string> imageId = None();

  // If the image specifies an id, use that. If not, then find the image in the
  // cache by its name and labels. Note that if store has the image, it has to
  // be in the cache. It is possible that an image could be found in cache but
  // not in the store due to eviction of the image from the store in between
  // cache restoration and now.

  if (appc.has_id()) {
    imageId = appc.id();
  } else {
    imageId = cache->find(appc);
  }

  if (imageId.isNone()) {
    return Failure("Failed to find image '" + appc.name() + "' in cache");
  }

  // Now validate the image path for the image. This will also check for the
  // existence of the directory.
  Option<Error> error =
    spec::validateLayout(paths::getImagePath(rootDir, imageId.get()));

  if (error.isSome()) {
    return Failure(
        "Failed to validate directory for image '" + appc.name() + "': " +
        error.get().message);
  }

  return ImageInfo{
      vector<string>({paths::getImageRootfsPath(rootDir, imageId.get())}),
      None()
  };
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
