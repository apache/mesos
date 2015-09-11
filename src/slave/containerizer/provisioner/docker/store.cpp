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

#include "slave/containerizer/provisioner/docker/local_store.hpp"
#include "slave/containerizer/provisioner/docker/store.hpp"

using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

Try<Owned<slave::Store>> Store::create(const Flags& flags)
{
  hashmap<string, Try<Owned<slave::Store>>(*)(const Flags&)> creators{
    {"local", &LocalStore::create}
  };

  if (!creators.contains(flags.docker_store_discovery)) {
    return Error("Unknown Docker store: " + flags.docker_store_discovery);
  }

  return creators[flags.docker_store_discovery](flags);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
