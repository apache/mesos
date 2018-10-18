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

#ifndef __MASTER_REGISTRAR_HPP__
#define __MASTER_REGISTRAR_HPP__

#include <mesos/mesos.hpp>

#include <mesos/state/state.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/hashset.hpp>

#include "master/flags.hpp"
#include "master/registry.hpp"

namespace mesos {
namespace internal {
namespace master {

// Forward declaration.
class RegistrarProcess;

// Defines an abstraction for operations that can be applied on the
// Registry.
// TODO(xujyan): Make RegistryOperation generic so that we can apply
// them against a generic "batch operation applier" abstraction, see
// the TODO below for more details.
class RegistryOperation : public process::Promise<bool>
{
public:
  // Attempts to invoke the operation on the registry object.
  // Aided by accumulator(s):
  //   slaveIDs - is the set of registered slaves.
  //
  // Returns whether the operation mutates 'registry', or an error if
  // the operation cannot be applied successfully.
  Try<bool> operator()(Registry* registry, hashset<SlaveID>* slaveIDs)
  {
    const Try<bool> result = perform(registry, slaveIDs);

    success = !result.isError();

    return result;
  }

  // Sets the promise based on whether the operation was successful.
  bool set() { return process::Promise<bool>::set(success); }

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) = 0;

private:
  bool success = false;
};


// TODO(xujyan): Add a generic abstraction for applying batches of
// operations against State Variables. The Registrar and other
// components could leverage this. This abstraction would be
// templatized to take the type, along with any accumulators:
// template <typename T,
//           typename X = None,
//           typename Y = None,
//           typename Z = None>
// T: the data type that the batch operations can be applied on.
// X, Y, Z: zero to 3 generic accumulators that facilitate the batch
// of operations.
// This allows us to reuse the idea of "doing batches of operations"
// on other potential new state variables (i.e. repair state, offer
// reservations, etc).
class Registrar
{
public:
  Registrar(const Flags& flags,
            mesos::state::State* state,
            const Option<std::string>& authenticationRealm = None());
  virtual ~Registrar();

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
  virtual process::Future<bool> apply(
      process::Owned<RegistryOperation> operation);

  // Gets the pid of the underlying process.
  // Used in tests.
  process::PID<RegistrarProcess> pid() const;

private:
  RegistrarProcess* process;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_REGISTRAR_HPP__
