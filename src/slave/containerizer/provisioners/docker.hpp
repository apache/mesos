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

#ifndef __MESOS_DOCKER_HPP__
#define __MESOS_DOCKER_HPP__

#include <list>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include <mesos/resources.hpp>

#include "slave/containerizer/provisioner.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declaration.
class Store;

/**
 * Represents Docker Image Name, which composes of a repository and a
 * tag.
 */
struct ImageName
{
  static Try<ImageName> create(const std::string& name);

  ImageName(
      const std::string& _repository,
      const std::string& _tag,
      const Option<std::string>& _registry = None())
    : repository(_repository),
      tag(_tag),
      registry(_registry) {}

  ImageName() {}

  /**
   * The string representation of this image.
   */
  std::string name() const
  {
    if (registry.isSome()) {
      return registry.get() +  "/" + repository + ":" + tag;
    }

    return repository + ":" + tag;
  }

  /**
   * Repository of this image (e.g, ubuntu).
   */
  std::string repository;

  /**
   * Tag of this image (e.g: 14.04).
   */
  std::string tag;

  /**
   * Custom registry that the image points to.
   */
  Option<std::string> registry;
};


inline std::ostream& operator<<(std::ostream& stream, const ImageName& image)
{
  return stream << image.name();
}


/**
 * Represents a Docker Image that holds its name and all of its layers
 * sorted by its dependency.
 */
struct DockerImage
{
  DockerImage() {}

  DockerImage(
      const ImageName& _imageName,
      const std::list<std::string>& _layerIds)
  : imageName(_imageName), layerIds(_layerIds) {}

  ImageName imageName;
  std::list<std::string> layerIds;
};

// Forward declaration.
class DockerProvisionerProcess;

/**
 * Docker Provisioner is responsible to provision rootfs for
 * containers with Docker images.
 */
class DockerProvisioner : public mesos::internal::slave::Provisioner
{
public:
  static Try<process::Owned<mesos::internal::slave::Provisioner>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual ~DockerProvisioner();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image);

  virtual process::Future<bool> destroy(const ContainerID& containerId);

private:
  explicit DockerProvisioner(process::Owned<DockerProvisionerProcess> _process);

  DockerProvisioner& operator=(const DockerProvisioner&) = delete; // Not assignable.
  DockerProvisioner(const DockerProvisioner&) = delete; // Not copyable.

  process::Owned<DockerProvisionerProcess> process;
};


} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_HPP__
