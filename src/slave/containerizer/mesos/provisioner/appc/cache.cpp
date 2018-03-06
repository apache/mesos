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

#include <boost/functional/hash.hpp>

#include <stout/os.hpp>

#include <mesos/appc/spec.hpp>

#include "slave/containerizer/mesos/provisioner/appc/cache.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"

using std::list;
using std::map;
using std::pair;
using std::string;

namespace spec = appc::spec;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

Try<process::Owned<Cache>> Cache::create(const Path& storeDir)
{
  // TODO(jojy): Should we create a directory if it does not exist ?
  if (!os::exists(storeDir)) {
    return Error(
        "Failed to find store directory '" + stringify(storeDir) + "'");
  }

  return new Cache(storeDir);
}


Cache::Cache(const Path& _storeDir)
  : storeDir(_storeDir) {}


Try<Nothing> Cache::recover()
{
  Try<list<string>> imageDirs = os::ls(paths::getImagesDir(storeDir));
  if (imageDirs.isError()) {
    return Error(
        "Failed to list images under '" +
        paths::getImagesDir(storeDir) + "': " +
        imageDirs.error());
  }

  foreach (const string& imageId, imageDirs.get()) {
    Try<Nothing> adding = add(imageId);
    if (adding.isError()) {
      LOG(WARNING) << "Failed to add image with id '" << imageId
                   << "' to cache: " << adding.error();
      continue;
    }

    LOG(INFO) << "Restored image with id '" << imageId << "'";
  }

  return Nothing();
}


Try<Nothing> Cache::add(const string& imageId)
{
  const string path = spec::getImageManifestPath(
      paths::getImagePath(storeDir, imageId));

  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error(
        "Failed to read manifest from '" + path + "': " + read.error());
  }

  Try<spec::ImageManifest> manifest = spec::parse(read.get());
  if (manifest.isError()) {
    return Error(
        "Failed to parse manifest from '" + path + "': " + manifest.error());
  }

  map<string, string> labels;
  foreach (const spec::ImageManifest::Label& label, manifest->labels()) {
    labels.insert({label.name(), label.value()});
  }

  imageIds.put(Key(manifest->name(), labels), imageId);

  VLOG(1) << "Added image with id '" << imageId << "' to cache";

  return Nothing();
}


Option<string> Cache::find(const Image::Appc& image) const
{
  // Create a cache key from image.
  Cache::Key key(image);

  if (!imageIds.contains(key)) {
    return None();
  }

  return imageIds.at(key);
}


Cache::Key::Key(const Image::Appc& image)
  : name(image.name())
{
  // Extract labels for easier comparison, this also weeds out duplicates.
  // TODO(xujyan): Detect duplicate labels in image manifest validation
  // and Image::Appc validation.

  // TODO(jojy): Do we need to normalize the labels by adding default labels
  // like os, version and arch if they are missing?
  foreach (const Label& label, image.labels().labels()) {
    labels.insert({label.key(), label.value()});
  }
}


Cache::Key::Key(
    const string& _name,
    const map<string, string>& _labels)
  : name(_name),
    labels(_labels) {}


bool Cache::Key::operator==(const Cache::Key& other) const
{
  if (name != other.name) {
    return false;
  }

  if (labels != other.labels) {
    return false;
  }

  return true;
}


size_t Cache::KeyHasher::operator()(const Cache::Key& key) const
{
  size_t seed = 0;

  boost::hash_combine(seed, key.name);
  boost::hash_combine(seed, key.labels);

  return seed;
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
