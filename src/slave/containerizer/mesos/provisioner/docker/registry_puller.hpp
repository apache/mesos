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

#include <list>
#include <string>
#include <utility>

#include <stout/duration.hpp>
#include <stout/path.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <mesos/docker/spec.hpp>

#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declarations.
class RegistryPullerProcess;

/*
 * Pulls an image from registry.
 */
class RegistryPuller : public Puller
{
public:
  /**
   * Factory method for creating RegistryPuller.
   */
  static Try<process::Owned<Puller>> create(const Flags& flags);

  ~RegistryPuller();

  /**
   * Pulls an image into a download directory. This image could consist
   * multiple layers of blobs.
   *
   * @param reference local name of the image.
   * @param downloadDir path to which the layers should be downloaded.
   */
  process::Future<std::list<std::pair<std::string, std::string>>> pull(
      const ::docker::spec::ImageReference& reference,
      const Path& downloadDir);

private:
  RegistryPuller(const process::Owned<RegistryPullerProcess>& process);

  process::Owned<RegistryPullerProcess> process_;

  RegistryPuller(const RegistryPuller&) = delete;
  RegistryPuller& operator=(const RegistryPuller&) = delete;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif //  __PROVISIONER_DOCKER_REGISTRY_PULLER_HPP__
