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

#ifndef __PROVISIONER_STORE_HPP__
#define __PROVISIONER_STORE_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/appc/spec.hpp>

#include <mesos/docker/v1.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashset.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Includes a vector of rootfs layers in topological order corresponding
// to a specific image, and its runtime configuration.
struct ImageInfo
{
  std::vector<std::string> layers;

  // Docker v1 image manifest.
  Option<::docker::spec::v1::ImageManifest> dockerManifest;

  // Appc image manifest.
  Option<::appc::spec::ImageManifest> appcManifest;

  // Path of docker manifest v2 schema2 config.
  Option<std::string> config;
};


// An image store abstraction that "stores" images. It serves as a
// read-through cache (cache misses are fetched remotely and
// transparently) for images.
class Store
{
public:
  static Try<hashmap<Image::Type, process::Owned<Store>>> create(
      const Flags& flags,
      SecretResolver* secretResolver = nullptr);

  virtual ~Store() {}

  virtual process::Future<Nothing> recover() = 0;

  // Get the specified image (and all its recursive dependencies) as a
  // list of rootfs layers in the topological order (dependencies go
  // before dependents in the list). The images required to build this
  // list are either retrieved from the local cache or fetched
  // remotely.
  //
  // NOTE: The returned list should not have duplicates. e.g., in the
  // following scenario the result should be [C, B, D, A] (B before D
  // in this example is decided by the order in which A specifies its
  // dependencies).
  //
  // A --> B --> C
  // |           ^
  // |---> D ----|
  //
  // The returned future fails if the requested image or any of its
  // dependencies cannot be found or failed to be fetched.
  virtual process::Future<ImageInfo> get(
      const Image& image,
      const std::string& backend) = 0;

  // Prune unused images from the given store. This is called within
  // an exclusive lock from `provisioner`, which means any other
  // image provision or prune are blocked until the future is satsified,
  // so an implementation should minimize the blocking time.
  //
  // Any image specified in `excludedImages` should not be pruned if
  // it is already cached previously.
  //
  // On top of this, all layer paths used to provisioner all active
  // containers are also passed in `activeLayerPaths`, and these layers
  // should also be retained. Because in certain store (e.g, docker store)
  // the cache is not source of truth, and we need to not only keep the
  // excluded images, but also maintain the cache.
  virtual process::Future<Nothing> prune(
      const std::vector<Image>& excludedImages,
      const hashset<std::string>& activeLayerPaths);
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_STORE_HPP__
