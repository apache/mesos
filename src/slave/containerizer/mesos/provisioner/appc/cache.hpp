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

#ifndef __APPC_PROVISIONER_CACHE_HPP__
#define __APPC_PROVISIONER_CACHE_HPP__

#include <map>
#include <string>

#include <process/owned.hpp>

#include <stout/hashmap.hpp>

#include <mesos/mesos.hpp>

#include <mesos/appc/spec.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

/**
 * Encapsulates Appc image cache.
 * Note: We only keep 1 level image information  and do not bother to keep
 * dependency information. This is because dependency graph will be resolved at
 * runtime (say when store->get is called).
 */
class Cache
{
public:
  /**
   * Factory method for creating cache.
   *
   * @param storeDir Path to the image store.
   * @returns Owned Cache pointer on success.
   *          Error on failure.
   */
  static Try<process::Owned<Cache>> create(const Path& storeDir);


  /**
   * Recovers/rebuilds the cache from its image store directory.
   */
  Try<Nothing> recover();


  /**
   * Adds an image to the cache by the image's id.
   *
   * Add is done in two steps:
   *  1.  A cache entry is created using the path constructed from store
   *      directory and the imageId parameter.
   *  2.  The cache entry is inserted into the cache.
   *
   * @param imageId Image id for the image that has to be added to the cache.
   * @returns Nothing on success.
   *          Error on failure to add.
   */
  Try<Nothing> add(const std::string& imageId);


  /**
   * Finds image id of an image if it is present in the cache/store.
   *
   * @param appc Appc image data.
   * @returns Image id of the image if its found in the cache.
   *          Error otherwise.
   */
  Option<std::string> find(const Image::Appc& image) const;

private:
  struct Key
  {
    Key(const Image::Appc& image);

    Key(const std::string& name,
        const std::map<std::string, std::string>& labels);

    bool operator==(const Key& other) const;

    std::string name;
    std::map<std::string, std::string> labels;
  };

  struct KeyHasher
  {
    size_t operator()(const Key& key) const;
  };

  Cache(const Path& imagesDir);

  const Path storeDir;

  // Mappings: Key -> image id.
  hashmap<Cache::Key, std::string, KeyHasher> imageIds;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __APPC_PROVISIONER_CACHE_HPP__
