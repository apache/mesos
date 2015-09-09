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

#ifndef __APPC_PROVISIONER_HPP__
#define __APPC_PROVISIONER_HPP__

#include <list>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/provisioner.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

// Forward declaration.
class AppcProvisionerProcess;


class AppcProvisioner : public Provisioner
{
public:
  static Try<process::Owned<Provisioner>> create(
      const Flags& flags,
      Fetcher* fetcher);

  ~AppcProvisioner();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image);

  virtual process::Future<bool> destroy(const ContainerID& containerId);

private:
  explicit AppcProvisioner(process::Owned<AppcProvisionerProcess> process);

  AppcProvisioner(const AppcProvisioner&); // Not copyable.
  AppcProvisioner& operator=(const AppcProvisioner&); // Not assignable.

  process::Owned<AppcProvisionerProcess> process;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __APPC_PROVISIONER_HPP__
