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

#ifndef __PROVISIONER_DOCKER_REGISTRY_PULLER_HPP__
#define __PROVISIONER_DOCKER_REGISTRY_PULLER_HPP__

#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/try.hpp>

#include <mesos/uri/fetcher.hpp>

#include <mesos/secret/resolver.hpp>

#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declarations.
class RegistryPullerProcess;

/*
 * Pulls an image from docker registry.
 */
class RegistryPuller : public Puller
{
public:
  static Try<process::Owned<Puller>> create(
      const Flags& flags,
      const process::Shared<uri::Fetcher>& fetcher,
      SecretResolver* secretResolver);

  ~RegistryPuller() override;

  /**
   * Pulls an image into a download directory. This image could
   * consist multiple layers of blobs.
   *
   * @param reference local name of the image.
   * @param directory path to which the layers will be downloaded.
   */
  process::Future<Image> pull(
      const ::docker::spec::ImageReference& reference,
      const std::string& directory,
      const std::string& backend,
      const Option<Secret>& config = None()) override;

private:
  RegistryPuller(process::Owned<RegistryPullerProcess> _process);

  RegistryPuller(const RegistryPuller&) = delete;
  RegistryPuller& operator=(const RegistryPuller&) = delete;

  process::Owned<RegistryPullerProcess> process;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif //  __PROVISIONER_DOCKER_REGISTRY_PULLER_HPP__
