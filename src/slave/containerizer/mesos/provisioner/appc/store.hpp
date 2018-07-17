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

#ifndef __PROVISIONER_APPC_STORE_HPP__
#define __PROVISIONER_APPC_STORE_HPP__

#include <mesos/secret/resolver.hpp>

#include "slave/containerizer/mesos/provisioner/store.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

// Forward declaration.
class StoreProcess;


class Store : public slave::Store
{
public:
  static Try<process::Owned<slave::Store>> create(
      const Flags& flags,
      SecretResolver* secretResolver = nullptr);

  ~Store() override;

  process::Future<Nothing> recover() override;

  // TODO(xujyan): Fetching remotely is not implemented for now and
  // until then the future fails directly if the image is not in the
  // local cache.
  // TODO(xujyan): The store currently doesn't support images that
  // have dependencies and we should add it later.
  process::Future<ImageInfo> get(
      const Image& image,
      const std::string& backend) override;

private:
  Store(process::Owned<StoreProcess> process);

  Store(const Store&) = delete; // Not copyable.
  Store& operator=(const Store&) = delete; // Not assignable.

  process::Owned<StoreProcess> process;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_APPC_STORE_HPP__
