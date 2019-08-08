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

#ifndef __PROVISIONER_BACKENDS_BIND_HPP__
#define __PROVISIONER_BACKENDS_BIND_HPP__

#include "slave/containerizer/mesos/provisioner/backend.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class BindBackendProcess;


// This is a specialized backend that may be useful for deployments
// using large (multi-GB) single-layer images *and* where more recent
// kernel features such as overlayfs are not available (overlayfs-based
// backend tracked by MESOS-2971). For small images (10's to 100's of MB)
// the copy backend may be sufficient. NOTE:
// 1) BindBackend supports only a single layer. Multi-layer images will
//    fail to provision and the container will fail to launch!
// 2) The filesystem is read-only because all containers using this
//    image share the source. Select writable areas can be achieved by
//    mounting read-write volumes to places like /tmp, /var/tmp,
//    /home, etc. using the ContainerInfo. These can be relative to
//    the executor work directory.
//    N.B. Since the filesystem is read-only:
//    i.  The '--sandbox_directory' must already exist within the
//        filesystem because the filesystem isolator is unable to
//        create it!
//    ii. The 'tmpfs' moint point '/tmp' must already exist within
//        the filesystem, because 'pivot_root' needs a mount point
//        for the old root.
// 3) It's fast because the bind mount requires (nearly) zero IO.
class BindBackend : public Backend
{
public:
  ~BindBackend() override;

  // BindBackend doesn't use any flag.
  static Try<process::Owned<Backend>> create(const Flags&);

  process::Future<Option<std::vector<Path>>> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs,
      const std::string& backendDir) override;

  process::Future<bool> destroy(
      const std::string& rootfs,
      const std::string& backendDir) override;

private:
  explicit BindBackend(process::Owned<BindBackendProcess> process);

  BindBackend(const BindBackend&); // Not copyable.
  BindBackend& operator=(const BindBackend&); // Not assignable.

  process::Owned<BindBackendProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_BACKENDS_BIND_HPP__
