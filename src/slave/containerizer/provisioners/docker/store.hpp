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

#ifndef __MESOS_DOCKER_STORE__
#define __MESOS_DOCKER_STORE__

#include <string>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/provisioners/docker.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Store fetches the Docker images and stores them on disk.
class Store
{
public:
  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual ~Store() {}

  /**
   * Put an image into the store. Returns the DockerImage containing
   * the manifest, hash of the image, and the path to the extracted
   * image.
   *
   * @param name The name of the Docker image being stored.
   * @param sandbox The path of the directory in which the stderr and
   *     stdout logs will be placed.
   *
   * @return The DockerImage placed in the store.
   */
  virtual process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox) = 0;

  /**
  * Get image by name.
  *
  * @param name The name of the Docker image to retrieve from store.
  *
  * @return The DockerImage or none if image is not in the store.
  */
  virtual process::Future<Option<DockerImage>> get(const std::string& name) = 0;

  // TODO(chenlily): Implement removing an image.

protected:
  Store() {}
};

// Forward Declaration.
class LocalStoreProcess;

class LocalStore : public Store
{
public:
  virtual ~LocalStore();

  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  /**
   * Put assumes the image tar archive is located in the directory specified in
   * the slave flag docker_discovery_local_dir and is named with <name>.tar .
   */
  virtual process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  virtual process::Future<Option<DockerImage>> get(const std::string& name);

private:
  explicit LocalStore(process::Owned<LocalStoreProcess> process);

  LocalStore(const LocalStore&); // Not copyable.

  LocalStore& operator=(const LocalStore&); // Not assignable.

  process::Owned<LocalStoreProcess> process;
};


class LocalStoreProcess : public process::Process<LocalStoreProcess>
{
public:
  ~LocalStoreProcess() {}

  static Try<process::Owned<LocalStoreProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  process::Future<Option<DockerImage>> get(const std::string& name);

private:
  LocalStoreProcess(const Flags& flags);

  Try<Nothing> restore(const Flags& flags);

  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& staging);

  process::Future<DockerImage> putImage(
      const std::string& name,
      const std::string& staging,
      const std::string& sandbox);

  Result<std::string> getParentId(
      const std::string& staging,
      const std::string& layerId);

  process::Future<Nothing> putLayers(
      const std::string& staging,
      const std::list<std::string>& layers,
      const std::string& sandbox);

  process::Future<Nothing> untarLayer(
      const std::string& staging,
      const std::string& id,
      const std::string& sandbox);

  process::Future<Nothing> moveLayer(
      const std::string& staging,
      const std::string& id,
      const std::string& sandbox);

  const Flags flags;

  // This hashmap maps a Docker image by name to its corresponding DockerImage
  // object.
  hashmap<std::string, DockerImage> images;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_STORE__
