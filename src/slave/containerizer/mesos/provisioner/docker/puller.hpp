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

#ifndef __PROVISIONER_DOCKER_PULLER_HPP__
#define __PROVISIONER_DOCKER_PULLER_HPP__

#include <list>
#include <utility>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include "slave/containerizer/mesos/provisioner/docker/message.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class Puller
{
public:
  static Try<process::Owned<Puller>> create(const Flags& flags);

  virtual ~Puller() {}

  /**
   * Pull a Docker image layers into the specified directory, and
   * return the list of layer ids in that image in the right
   * dependency order, and also return the directory where
   * the puller puts its changeset.
   *
   * @param name The name of the image.
   * @param directory The target directory to store the layers.
   * @return list of layers maped to its local directory ordered by its
   *         dependency.
   */
  virtual process::Future<std::list<std::pair<std::string, std::string>>> pull(
      const docker::Image::Name& name,
      const Path& directory) = 0;
};


/**
 * Untars(extracts) the tar file(input param) to the given output directory.
 *
 * @param file tar file to be extracted.
 * @param directory target directory for extracting the tar file.
 */
process::Future<Nothing> untar(
    const std::string& file,
    const std::string& directory);


/**
 * Untars a tarred layer changeset into staging directory with the
 * directory structure:
 *    |--staging directory
 *        |-- <layer_id>
 *            |-- rootfs
 *
 * @param file path to the tar file holding the Docker layer.
 * @param directory staging directory.
 * @return layer Id mapping to the rootfs path of the layer.
 */
process::Future<std::pair<std::string, std::string>> untarLayer(
    const std::string& file,
    const std::string& directory,
    const std::string& layerId);

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {


#endif // __PROVISIONER_DOCKER_PULLER_HPP__
