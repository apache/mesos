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

#ifndef __PROVISIONER_HPP__
#define __PROVISIONER_HPP__

#include <list>

#include <mesos/resources.hpp>

#include <mesos/slave/isolator.hpp> // For ContainerState.

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/fetcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class ProvisionerProcess;


class Provisioner
{
public:
  // Create the provisioner based on the specified flags.
  static Try<process::Owned<Provisioner>> create(
      const Flags& flags,
      Fetcher* fetcher);

  // NOTE: Made 'virtual' for mocking and testing.
  virtual ~Provisioner();

  // Recover root filesystems for containers from the run states and
  // the orphan containers (known to the launcher but not known to the
  // slave) detected by the launcher. This function is also
  // responsible for cleaning up any intermediate artifacts (e.g.
  // directories) to not leak anything.
  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  // Provision a root filesystem for the container using the specified
  // image and return the absolute path to the root filesystem.
  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image);

  // Destroy a previously provisioned root filesystem. Assumes that
  // all references (e.g., mounts, open files) to the provisioned
  // filesystem have been removed. Return false if there is no
  // provisioned root filesystem for the given container.
  virtual process::Future<bool> destroy(const ContainerID& containerId);

protected:
  Provisioner() {} // For creating mock object.

private:
  explicit Provisioner(process::Owned<ProvisionerProcess> process);

  Provisioner(const Provisioner&) = delete; // Not copyable.
  Provisioner& operator=(const Provisioner&) = delete; // Not assignable.

  process::Owned<ProvisionerProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_HPP__
