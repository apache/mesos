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

#ifndef __MESOS_PROVISIONER_OVERLAY_HPP__
#define __MESOS_PROVISIONER_OVERLAY_HPP__

#include "slave/containerizer/mesos/provisioner/backend.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class OverlayBackendProcess;


// This is a specialized backend that is useful for deploying multi-layer
// images using the overlayfs-based backend.
// 1) OverlayBackend does not support images with a single layer.
// 2) The filesystem is read-only.
class OverlayBackend : public Backend
{
public:
  virtual ~OverlayBackend();

  static Try<process::Owned<Backend>> create(const Flags&);

  virtual process::Future<Nothing> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs);

  virtual process::Future<bool> destroy(const std::string& rootfs);

private:
  explicit OverlayBackend(process::Owned<OverlayBackendProcess> process);

  OverlayBackend(const OverlayBackend&);
  OverlayBackend& operator=(const OverlayBackend&);

  process::Owned<OverlayBackendProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_PROVISIONER_OVERLAY_HPP__
