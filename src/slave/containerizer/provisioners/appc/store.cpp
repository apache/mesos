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

#include <list>

#include <glog/logging.h>

#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "slave/containerizer/provisioners/appc/paths.hpp"
#include "slave/containerizer/provisioners/appc/spec.hpp"
#include "slave/containerizer/provisioners/appc/store.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(const string& root);

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<std::vector<Store::Image>> get(const string& name);

private:
  // Absolute path to the root directory of the store as defined by
  // --appc_store_dir.
  const string root;

  // Mappings: name -> id -> image.
  hashmap<string, hashmap<string, Store::Image>> images;
};


Try<Owned<Store>> Store::create(const Flags& flags)
{
  Try<Nothing> mkdir = os::mkdir(paths::getImagesDir(flags.appc_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create the images directory: " + mkdir.error());
  }

  // Make sure the root path is canonical so all image paths derived
  // from it are canonical too.
  Result<string> root = os::realpath(flags.appc_store_dir);
  if (!root.isSome()) {
    // The above mkdir call recursively creates the store directory
    // if necessary so it cannot be None here.
    CHECK_ERROR(root);
    return Error(
        "Failed to get the realpath of the store directory: " + root.error());
  }

  return Owned<Store>(new Store(
      Owned<StoreProcess>(new StoreProcess(root.get()))));
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


Future<vector<Store::Image>> Store::get(const string& name)
{
  return dispatch(process.get(), &StoreProcess::get, name);
}


StoreProcess::StoreProcess(const string& _root) : root(_root) {}


// Implemented as a helper function because it's going to be used by 'put()'.
static Try<Store::Image> createImage(const string& imagePath)
{
  Option<Error> error = spec::validateLayout(imagePath);
  if (error.isSome()) {
    return Error("Invalid image layout: " + error.get().message);
  }

  string imageId = Path(imagePath).basename();

  error = spec::validateImageID(imageId);
  if (error.isSome()) {
    return Error("Invalid image ID: " + error.get().message);
  }

  Try<string> read = os::read(paths::getImageManifestPath(imagePath));
  if (read.isError()) {
    return Error("Failed to read manifest: " + read.error());
  }

  Try<AppcImageManifest> manifest = spec::parse(read.get());
  if (manifest.isError()) {
    return Error("Failed to parse manifest: " + manifest.error());
  }

  return Store::Image(manifest.get(), imageId, imagePath);
}


Future<vector<Store::Image>> StoreProcess::get(const string& name)
{
  if (!images.contains(name)) {
    return vector<Store::Image>();
  }

  vector<Store::Image> result;
  foreach (const Store::Image& image, images[name].values()) {
    result.push_back(image);
  }

  return result;
}


Future<Nothing> StoreProcess::recover()
{
  // Recover everything in the store.
  Try<list<string>> imageIds = os::ls(paths::getImagesDir(root));
  if (imageIds.isError()) {
    return Failure(
        "Failed to list images under '" +
        paths::getImagesDir(root) + "': " +
        imageIds.error());
  }

  foreach (const string& imageId, imageIds.get()) {
    string path = paths::getImagePath(root, imageId);
    if (!os::stat::isdir(path)) {
      LOG(WARNING) << "Unexpected entry in storage: " << imageId;
      continue;
    }

    Try<Store::Image> image = createImage(path);
    if (image.isError()) {
      LOG(WARNING) << "Unexpected entry in storage: " << image.error();
      continue;
    }

    LOG(INFO) << "Restored image '" << image.get().manifest.name() << "'";

    images[image.get().manifest.name()].put(image.get().id, image.get());
  }

  return Nothing();
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
