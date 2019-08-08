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

#ifndef __PROVISIONER_BACKENDS_COPY_HPP__
#define __PROVISIONER_BACKENDS_COPY_HPP__

#include "slave/containerizer/mesos/provisioner/backend.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class CopyBackendProcess;


// The backend implementation that copies the layers to the target.
// NOTE: Using this backend currently has a few implications:
// 1) The disk space used by the provisioned rootfs is not counted
//    towards either the usage by the executor/task or the store
//    cache, which can interfere with the slave hosts's disk space
//    allocation.
// 2) The task can write unrestrictedly into the provisioned rootfs
//    which is not accounted for (in terms of disk usage) either.
class CopyBackend : public Backend
{
public:
  ~CopyBackend() override;

  // CopyBackend doesn't use any flag.
  static Try<process::Owned<Backend>> create(const Flags&);

  // Provisions a rootfs given the layers' paths and target rootfs
  // path.
  process::Future<Option<std::vector<Path>>> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs,
      const std::string& backendDir) override;

  process::Future<bool> destroy(
      const std::string& rootfs,
      const std::string& backendDir) override;

private:
  explicit CopyBackend(process::Owned<CopyBackendProcess> process);

  CopyBackend(const CopyBackend&); // Not copyable.
  CopyBackend& operator=(const CopyBackend&); // Not assignable.

  process::Owned<CopyBackendProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_BACKENDS_COPY_HPP__
