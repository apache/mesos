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

#include "slave/containerizer/provisioner/store.hpp"

#include "slave/containerizer/provisioner/docker/message.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Store fetches the Docker images and stores them on disk.
// TODO(tnachen): Make this store the only docker store
// implementation, and move local and remote to different pullers.
class Store
{
public:
  static Try<process::Owned<slave::Store>> create(const Flags& flags);
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_STORE_HPP__
