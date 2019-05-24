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

#include <mesos/type_utils.hpp>

#include <mesos/secret/resolver.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/mesos/provisioner/store.hpp"

#ifndef __WINDOWS__
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

#include "slave/containerizer/mesos/provisioner/docker/store.hpp"
#endif // __WINDOWS__

using std::string;
using std::vector;

using process::Owned;

namespace mesos {
namespace internal {
namespace slave {

Try<hashmap<Image::Type, Owned<Store>>> Store::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
#ifdef __WINDOWS__
  if (!flags.image_providers.isNone()) {
    return Error("Image stores are not supported on Windows");
  }

  return hashmap<Image::Type, Owned<Store>>();
#else
  if (flags.image_providers.isNone()) {
    return hashmap<Image::Type, Owned<Store>>();
  }

  hashmap<Image::Type, Try<Owned<Store>>(*)(
      const Flags&, SecretResolver*)> creators;

  creators.put(Image::APPC, &appc::Store::create);
  creators.put(Image::DOCKER, &docker::Store::create);

  hashmap<Image::Type, Owned<Store>> stores;

  foreach (const string& type,
           strings::tokenize(flags.image_providers.get(), ",")) {
    Image::Type imageType;
    if (!Image::Type_Parse(strings::upper(type), &imageType)) {
      return Error("Unknown image type '" + type + "'");
    }

    if (!creators.contains(imageType)) {
      return Error("Unsupported image type '" + type + "'");
    }

    Try<Owned<Store>> store = creators[imageType](flags, secretResolver);
    if (store.isError()) {
      return Error(
          "Failed to create store for image type '" +
          type + "': " + store.error());
    }

    stores.put(imageType, store.get());
  }

  if (stores.contains(Image::DOCKER) &&
      !strings::contains(flags.isolation, "docker/runtime")) {
    EXIT(EXIT_FAILURE)
      << "Docker runtime isolator has to be specified if 'DOCKER' is included"
      << " in 'image_providers'. Please add 'docker/runtime' to '--isolation'"
      << " flags";
  }

  return stores;
#endif // __WINDOWS__
}


process::Future<Nothing> Store::prune(
    const vector<Image>& excludeImages,
    const hashset<string>& activeLayerPaths)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
