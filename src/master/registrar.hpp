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

#ifndef __MASTER_REGISTRAR_HPP__
#define __MASTER_REGISTRAR_HPP__

#include <mesos/mesos.hpp>

#include <process/future.hpp>

#include "master/flags.hpp"
#include "master/registry.hpp"

#include "state/protobuf.hpp"

namespace mesos {
namespace internal {
namespace master {

// Forward declaration.
class RegistrarProcess;

class Registrar
{
public:
  // If flags.registry_strict is true, all operations will be
  // permitted.
  Registrar(const Flags& flags, state::protobuf::State* state);
  ~Registrar();

  // Recovers the Registry, persisting the new Master information.
  // The Registrar must be recovered to allow other operations to
  // proceed.
  // TODO(bmahler): Consider a "factory" for constructing the
  // Registrar, to eliminate the need for passing 'MasterInfo'.
  // This is required as the Registrar is injected into the Master,
  // and therefore MasterInfo is unknown during construction.
  process::Future<Registry> recover(const MasterInfo& info);

  // The following are operations that can be performed on the
  // Registry for a slave. Returns:
  //   true if the operation is permitted.
  //   false if the operation is not permitted.
  //   Failure if the operation fails (possibly lost log leadership),
  //     recovery failed, or if 'info' is missing an ID.
  process::Future<bool> admit(const SlaveInfo& info);
  process::Future<bool> readmit(const SlaveInfo& info);
  process::Future<bool> remove(const SlaveInfo& info);

private:
  RegistrarProcess* process;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_REGISTRAR_HPP__
