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

#ifndef __PROVISIONER_DOCKER_STORE_HPP__
#define __PROVISIONER_DOCKER_STORE_HPP__

#include <mesos/secret/resolver.hpp>

#include <process/owned.hpp>

#include <stout/hashset.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/provisioner/store.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward Declarations.
class Puller;
class StoreProcess;


// Store fetches the Docker images and stores them on disk.
class Store : public slave::Store
{
public:
  static Try<process::Owned<slave::Store>> create(
      const Flags& flags,
      SecretResolver* secretResolver = nullptr);

  // This allows the puller to be mocked for testing.
  static Try<process::Owned<slave::Store>> create(
      const Flags& flags,
      const process::Owned<Puller>& puller);

  ~Store() override;

  process::Future<Nothing> recover() override;

  process::Future<ImageInfo> get(
      const mesos::Image& image,
      const std::string& backend) override;

  process::Future<Nothing> prune(
      const std::vector<mesos::Image>& excludeImages,
      const hashset<std::string>& activeLayerPaths) override;

private:
  explicit Store(process::Owned<StoreProcess> process);

  Store& operator=(const Store&) = delete; // Not assignable.
  Store(const Store&) = delete; // Not copyable.

  process::Owned<StoreProcess> process;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_STORE_HPP__
