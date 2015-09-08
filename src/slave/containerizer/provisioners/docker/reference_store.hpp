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

#ifndef __MESOS_DOCKER_REFERENCE_STORE_HPP__
#define __MESOS_DOCKER_REFERENCE_STORE_HPP__

#include <list>
#include <string>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include "slave/containerizer/provisioners/docker.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward Declaration.
class ReferenceStoreProcess;

/**
 * The Reference Store is a way to track the Docker images used by the
 * provisioner that are stored in on disk. It keeps track of the layers
 * that Docker images are composed of and recovers DockerImage objects upon
 * initialization by checking for dependent layers stored on disk.
 * Currently, image layers are stored indefinitely, with no garbage collection
 * of unreferenced image layers.
 */
class ReferenceStore
{
public:
  ~ReferenceStore();

  static Try<process::Owned<ReferenceStore>> create(const Flags& flags);

  /**
   * Create a DockerImage, put it in reference store and persist the reference
   * store state to disk.
   *
   * @param name   the name of the Docker image to place in the reference store.
   * @param layers the list of layer ids that comprise the Docker image in
   *               order where the root layer's id (no parent layer) is first
   *               and the leaf layer's id is last.
   */
  process::Future<DockerImage> put(
      const std::string& name,
      const std::list<std::string>& layers);

  /**
   * Retrieve DockerImage based on image name if it is among the DockerImages
   * stored in memory.
   *
   * @param name  the name of the Docker image to retrieve
   */
  process::Future<Option<DockerImage>> get(const std::string& name);

  /**
   * Recover all stored DockerImage and its layer references.
   */
  process::Future<Nothing> recover();

private:
  explicit ReferenceStore(process::Owned<ReferenceStoreProcess> process);

  ReferenceStore(const ReferenceStore&); // Not copyable.
  ReferenceStore& operator=(const ReferenceStore&); // Not assignable.

  process::Owned<ReferenceStoreProcess> process;
};


} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_REFERENCE_STORE_HPP__
