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

#ifndef __VOLUME_GID_MANAGER_HPP__
#define __VOLUME_GID_MANAGER_HPP__

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

#ifndef __WINDOWS__
#include "slave/volume_gid_manager/state.hpp"
#endif // __WINDOWS__

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class VolumeGidManagerProcess;


// Manages the allocation of owner group IDs for shared
// persistent volumes and SANDBOX_PATH volume of PARENT type.
class VolumeGidManager
{
#ifndef __WINDOWS__
public:
  static Try<VolumeGidManager*> create(const Flags& flags);

  ~VolumeGidManager();

  process::Future<Nothing> recover(bool rebooted) const;

  process::Future<gid_t> allocate(
      const std::string& path,
      VolumeGidInfo::Type type) const;

  process::Future<Nothing> deallocate(const std::string& path) const;

private:
  explicit VolumeGidManager(
      const process::Owned<VolumeGidManagerProcess>& process);

  process::Owned<VolumeGidManagerProcess> process;
#endif // __WINDOWS__
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __VOLUME_GID_MANAGER_HPP__
