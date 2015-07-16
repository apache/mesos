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

#ifndef __MESOS_PROVISIONER_HPP__
#define __MESOS_PROVISIONER_HPP__

#include <list>

#include <mesos/resources.hpp>

#include <mesos/slave/isolator.hpp> // For ExecutorRunState.

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/fetcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Provisioner
{
public:
  virtual ~Provisioner() {}

  static Try<hashmap<ContainerInfo::Image::Type, process::Owned<Provisioner>>>
    create(const Flags& flags, Fetcher* fetcher);

  // Recover root filesystems for containers from the run states and
  // the orphan containers (known to the launcher but not known to the
  // slave) detected by the launcher. This function is also
  // responsible for cleaning up any intermediate artifacts (e.g.
  // directories) to not leak anything.
  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states,
      const hashset<ContainerID>& orphans) = 0;

  // Provision a root filesystem for the container using the specified
  // image and return the absolute path to the root filesystem.
  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const ContainerInfo::Image& image) = 0;

  // Destroy a previously provisioned root filesystem. Assumes that
  // all references (e.g., mounts, open files) to the provisioned
  // filesystem have been removed.
  virtual process::Future<Nothing> destroy(const ContainerID& containerId) = 0;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_PROVISIONER_HPP__
