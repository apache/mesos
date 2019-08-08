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

#ifndef __PROVISIONER_BACKEND_HPP__
#define __PROVISIONER_BACKEND_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Provision a root filesystem for a container.
class Backend
{
public:
  virtual ~Backend() {}

  // Return a map of all supported backends keyed by their names. Note
  // that Backends that failed to be created due to incorrect flags are
  // simply not added to the result.
  static hashmap<std::string, process::Owned<Backend>> create(
      const Flags& flags);

  // Provision a root filesystem for a container into the specified 'rootfs'
  // directory by applying the specified list of root filesystem layers in
  // the list order, i.e., files in a layer can overwrite/shadow those from
  // another layer earlier in the list.
  //
  // Optionally returns a set of paths whose contents should be included
  // in the ephemeral sandbox disk quota.
  virtual process::Future<Option<std::vector<Path>>> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs,
      const std::string& backendDir) = 0;

  // Destroy the root filesystem provisioned at the specified 'rootfs'
  // directory. Return false if there is no provisioned root filesystem
  // to destroy for the given directory.
  virtual process::Future<bool> destroy(
      const std::string& rootfs,
      const std::string& backendDir) = 0;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_BACKEND_HPP__
