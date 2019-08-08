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

#ifndef __MESOS_PROVISIONER_AUFS_HPP__
#define __MESOS_PROVISIONER_AUFS_HPP__

#include "slave/containerizer/mesos/provisioner/backend.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class AufsBackendProcess;


// This backend mounts the images layers to the rootfs using the aufs file
// system. The directory layout is as follows:
// <work_dir> ('--work_dir' flag)
// |-- provisioner
//     |-- containers
//         |-- <container-id>
//             |-- backends
//                 |-- aufs
//                    |-- rootfses
//                        |-- <rootfs_id> (the rootfs)
//                    |-- scratch
//                        |-- <rootfs_id> (the scratch space)
//                            |-- workdir
//                            |-- links (symlink to temp dir with links to layers) // NOLINT(whitespace/line_length)
class AufsBackend : public Backend
{
public:
  ~AufsBackend() override;

  static Try<process::Owned<Backend>> create(const Flags&);

  process::Future<Option<std::vector<Path>>> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs,
      const std::string& backendDir) override;

  process::Future<bool> destroy(
      const std::string& rootfs,
      const std::string& backendDir) override;

private:
  explicit AufsBackend(process::Owned<AufsBackendProcess> process);

  AufsBackend(const AufsBackend&);
  AufsBackend& operator=(const AufsBackend&);

  process::Owned<AufsBackendProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_PROVISIONER_AUFS_HPP__
