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

#include "slave/containerizer/provisioner/appc/paths.hpp"
#include "slave/containerizer/provisioner/appc/spec.hpp"
#include "slave/containerizer/provisioner/appc/store.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

// Defines a locally cached image (which has passed validation).
struct CachedImage
{
  static Try<CachedImage> create(const string& imagePath);

  CachedImage(
      const AppcImageManifest& _manifest,
      const string& _id,
      const string& _path)
    : manifest(_manifest), id(_id), path(_path) {}

  string rootfs() const
  {
    return path::join(path, "rootfs");
  }

  const AppcImageManifest manifest;

  // Image ID of the format "sha512-value" where "value" is the hex
  // encoded string of the sha512 digest of the uncompressed tar file
  // of the image.
  const string id;

  // Absolute path to the extracted image.
  const string path;
};


Try<CachedImage> CachedImage::create(const string& imagePath)
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

  return CachedImage(manifest.get(), imageId, imagePath);
}


// Helper that implements this:
// https://github.com/appc/spec/blob/master/spec/aci.md#dependency-matching
static bool matches(Image::Appc requirements, const CachedImage& candidate)
{
  // The name must match.
  if (candidate.manifest.name() != requirements.name()) {
    return false;
  }

  // If an id is specified the candidate must match.
  if (requirements.has_id() && (candidate.id != requirements.id())) {
    return false;
  }

  // Extract labels for easier comparison, this also weeds out duplicates.
  // TODO(xujyan): Detect duplicate labels in image manifest validation
  // and Image::Appc validation.
  hashmap<string, string> requiredLabels;
  foreach (const Label& label, requirements.labels().labels()) {
    requiredLabels[label.key()] = label.value();
  }

  hashmap<string, string> candidateLabels;
  foreach (const AppcImageManifest::Label& label,
           candidate.manifest.labels()) {
    candidateLabels[label.name()] = label.value();
  }

  // Any label specified must be present and match in the candidate.
  foreachpair (const string& name,
               const string& value,
               requiredLabels) {
    if (!candidateLabels.contains(name) ||
        candidateLabels.get(name).get() != value) {
      return false;
    }
  }

  return true;
}


class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(const string& rootDir);

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<vector<string>> get(const Image& image);

private:
  // Absolute path to the root directory of the store as defined by
  // --appc_store_dir.
  const string rootDir;

  // Mappings: name -> id -> image.
  hashmap<string, hashmap<string, CachedImage>> images;
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

  return Owned<slave::Store>(new Store(
      Owned<StoreProcess>(new StoreProcess(rootDir.get()))));
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


Future<vector<string>> Store::get(const Image& image)
{
  return dispatch(process.get(), &StoreProcess::get, image);
}


StoreProcess::StoreProcess(const string& _rootDir) : rootDir(_rootDir) {}


Future<Nothing> StoreProcess::recover()
{
  // Recover everything in the store.
  Try<list<string>> imageIds = os::ls(paths::getImagesDir(rootDir));
  if (imageIds.isError()) {
    return Failure(
        "Failed to list images under '" +
        paths::getImagesDir(rootDir) + "': " +
        imageIds.error());
  }

  foreach (const string& imageId, imageIds.get()) {
    string path = paths::getImagePath(rootDir, imageId);
    if (!os::stat::isdir(path)) {
      LOG(WARNING) << "Unexpected entry in storage: " << imageId;
      continue;
    }

    Try<CachedImage> image = CachedImage::create(path);
    if (image.isError()) {
      LOG(WARNING) << "Unexpected entry in storage: " << image.error();
      continue;
    }

    LOG(INFO) << "Restored image '" << image.get().manifest.name() << "'";

    images[image.get().manifest.name()].put(image.get().id, image.get());
  }

  return Nothing();
}


Future<vector<string>> StoreProcess::get(const Image& image)
{
  if (image.type() != Image::APPC) {
    return Failure("Not an Appc image: " + stringify(image.type()));
  }

  const Image::Appc& appc = image.appc();

  if (!images.contains(appc.name())) {
    return Failure("No Appc image named '" + appc.name() + "' can be found");
  }

  // Get local candidates.
  vector<CachedImage> candidates;
  foreach (const CachedImage& candidate, images[appc.name()].values()) {
    // The first match is returned.
    // TODO(xujyan): Some tie-breaking rules are necessary.
    if (matches(appc, candidate)) {
      LOG(INFO) << "Found match for Appc image '" << appc.name()
                << "' in the store";

      // The Appc store current doesn't support dependencies and this
      // is enforced by manifest validation: if the image's manifest
      // contains dependencies it would fail the validation and
      // wouldn't be stored in the store.
      return vector<string>({candidate.rootfs()});
    }
  }

  return Failure("No Appc image named '" + appc.name() +
                 "' can match the requirements");
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
