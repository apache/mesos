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

#ifndef __MESOS_DOCKER_STORE_HPP__
#define __MESOS_DOCKER_STORE_HPP__

#include <string>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>

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
   * Get image by name.
   *
   * @param name The name of the Docker image to retrieve from store.
   *
   * @return The DockerImage that holds the Docker layers.
   */
  virtual process::Future<DockerImage> get(const ImageName& name) = 0;

  /**
   * Recover all stored images
   */
  virtual process::Future<Nothing> recover() = 0;

  // TODO(chenlily): Implement removing an image.

protected:
  Store() {}
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_STORE_HPP__
