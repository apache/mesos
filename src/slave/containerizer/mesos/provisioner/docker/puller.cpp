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

#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/provisioner/docker/local_puller.hpp"
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
    const Shared<uri::Fetcher>& fetcher)
{
  // TODO(tnachen): Support multiple registries in the puller.
  if (strings::startsWith(flags.docker_registry, "/")) {
    Try<Owned<Puller>> puller = LocalPuller::create(flags);
    if (puller.isError()) {
      return Error("Failed to create local puller: " + puller.error());
    }

    return puller.get();
  }

  Try<Owned<Puller>> puller = RegistryPuller::create(flags, fetcher);
  if (puller.isError()) {
    return Error("Failed to create registry puller: " + puller.error());
  }

  return puller.get();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
