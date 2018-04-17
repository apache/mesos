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

#include <mesos/secret/resolver.hpp>

#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/provisioner/docker/image_tar_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

using process::Owned;
using process::Shared;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

Try<Owned<Puller>> Puller::create(
    const Flags& flags,
    const Shared<uri::Fetcher>& fetcher,
    SecretResolver* secretResolver)
{
  // TODO(gilbert): Consider to introduce a new protobuf API to
  // represent docker image by an optional URI in Image::Docker,
  // so that the source of docker images are not necessarily from
  // the agent flag.
  // TODO(gilbert): Support multiple pullers simultaneously in
  // docker store, so that users could prefer pulling from either
  // image tarballs or the remote docker registry.
  if (strings::startsWith(flags.docker_registry, "/") ||
      strings::startsWith(flags.docker_registry, "hdfs://")) {
    Try<Owned<Puller>> puller = ImageTarPuller::create(flags, fetcher);
    if (puller.isError()) {
      return Error("Failed to create image tar puller " + puller.error());
    }

    return puller.get();
  }

  Try<Owned<Puller>> puller =
    RegistryPuller::create(flags, fetcher, secretResolver);

  if (puller.isError()) {
    return Error("Failed to create registry puller: " + puller.error());
  }

  return puller.get();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
