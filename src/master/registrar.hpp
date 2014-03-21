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
#include <process/owned.hpp>

#include "master/flags.hpp"
#include "master/registry.hpp"

#include "state/protobuf.hpp"

namespace mesos {
namespace internal {
namespace master {

// Forward declaration.
class RegistrarProcess;

// Defines an abstraction for operations that can be applied on the
// Registry.
class Operation : public process::Promise<bool>
{
public:
  Operation() : success(false) {}

  // Attempts to invoke the operation on 't'.
  // Returns whether the operation mutates 't', or an error if the
  // operation cannot be applied successfully.
  Try<bool> operator () (Registry* registry, bool strict)
  {
    const Try<bool>& result = perform(registry, strict);

    success = !result.isError();

    return result;
  }

  // Sets the promise based on whether the operation was successful.
  bool set() { return process::Promise<bool>::set(success); }

protected:
  virtual Try<bool> perform(Registry* registry, bool strict) = 0;

private:
  bool success;
};


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

  // Applies an operation on the Registry.
  // Returns:
  //   true if the operation is permitted.
  //   false if the operation is not permitted.
  //   Failure if the operation fails (possibly lost log leadership),
  //     or recovery failed.
  process::Future<bool> apply(process::Owned<Operation> operation);

private:
  RegistrarProcess* process;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_REGISTRAR_HPP__
