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

#ifndef __MESOS_DOCKER_LOCAL_STORE_HPP__
#define __MESOS_DOCKER_LOCAL_STORE_HPP__

#include "slave/containerizer/provisioners/docker/store.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declaration.
class LocalStoreProcess;


/**
 * LocalStore assumes Docker images are stored in a local directory
 * (configured with flags.docker_discovery_local_dir), with all the
 * images saved as tar with the name as the image name with tag (e.g:
 * ubuntu:14.04.tar).
 */
class LocalStore : public Store
{
public:
  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual ~LocalStore();

  virtual process::Future<DockerImage> get(const ImageName& name);

  virtual process::Future<Nothing> recover();

private:
  explicit LocalStore(process::Owned<LocalStoreProcess> _process);

  LocalStore& operator=(const LocalStore&) = delete; // Not assignable.
  LocalStore(const LocalStore&) = delete; // Not copyable.

  process::Owned<LocalStoreProcess> process;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_LOCAL_STORE_HPP__
