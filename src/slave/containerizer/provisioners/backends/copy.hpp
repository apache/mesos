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

#ifndef __MESOS_PROVISIONER_COPY_HPP__
#define __MESOS_PROVISIONER_COPY_HPP__

#include "slave/containerizer/provisioners/backend.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class CopyBackendProcess;


class CopyBackend : public Backend
{
public:
  virtual ~CopyBackend();

  // CopyBackend doesn't use any flag.
  static Try<process::Owned<Backend>> create(const Flags&);

  // Provisions a rootfs given the layers' paths and target rootfs
  // path.
  virtual process::Future<Nothing> provision(
      const std::vector<std::string>& layers,
      const std::string& rootfs);

  virtual process::Future<bool> destroy(const std::string& rootfs);

private:
  explicit CopyBackend(process::Owned<CopyBackendProcess> process);

  CopyBackend(const CopyBackend&); // Not copyable.
  CopyBackend& operator=(const CopyBackend&); // Not assignable.

  process::Owned<CopyBackendProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_PROVISIONER_COPY_HPP__
