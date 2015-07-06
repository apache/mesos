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

  // Put an image into to the store. Returns the DockerImage containing
  // the manifest, hash of the image, and the path to the extracted
  // image.
  virtual process::Future<DockerImage> put(
      const std::string& uri,
      const std::string& name,
      const std::string& directory) = 0;

  // Get image by name.
  virtual process::Future<Option<DockerImage>> get(const std::string& name) = 0;

protected:
  Store() {}
};


// Forward declaration.
class LocalStoreProcess;

class LocalStore : public Store
{
public:
  virtual ~LocalStore();

  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual process::Future<DockerImage> put(
      const std::string& uri,
      const std::string& name,
      const std::string& directory);

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
      const std::string& uri,
      const std::string& name,
      const std::string& directory);

  process::Future<Option<DockerImage>> get(const std::string& name);

private:
  LocalStoreProcess(
      const Flags& flags,
      Fetcher* fetcher);

  Try<Nothing> restore();

  process::Future<process::Shared<DockerLayer>> putLayer(
      const std::string& uri,
      const std::string& directory);

  process::Future<Nothing> untarLayer(
      const std::string& uri);

  process::Future<process::Shared<DockerLayer>> storeLayer(
      const std::string& hash,
      const std::string& uri,
      const std::string& directory);

  process::Future<process::Shared<DockerLayer>> entry(
      const std::string& uri,
      const std::string& directory);

  const Flags flags;

  // name -> DockerImage
  hashmap<std::string, DockerImage> images;
  // hash -> DockerLayer
  hashmap<std::string, process::Shared<DockerLayer>> layers;

  Fetcher* fetcher;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_STORE__
