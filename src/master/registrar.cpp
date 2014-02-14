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

#include <deque>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "common/type_utils.hpp"

#include "master/registrar.hpp"
#include "master/registry.hpp"

#include "state/protobuf.hpp"

using mesos::internal::state::protobuf::State;
using mesos::internal::state::protobuf::Variable;

using process::dispatch;
using process::Failure;
using process::Future;
using process::Process;
using process::Promise;
using process::spawn;
using process::terminate;
using process::wait; // Necessary on some OS's to disambiguate.

using std::deque;
using std::string;

namespace mesos {
namespace internal {
namespace master {

class RegistrarProcess : public Process<RegistrarProcess>
{
public:
  RegistrarProcess(State* _state)
    : ProcessBase("registrar"),
      updating(false),
      state(_state) {}

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<bool> admit(const SlaveInfo& info);
  Future<bool> readmit(const SlaveInfo& info);
  Future<bool> remove(const SlaveInfo& info);

private:
  template <typename T>
  struct Mutation : process::Promise<bool>
  {
    virtual Try<T> apply(T t) = 0;
  };

  struct Admit : Mutation<Registry>
  {
    Admit(const SlaveInfo& _info) : info(_info) {}

    virtual Try<Registry> apply(Registry registry)
    {
      // Check and see if this slave already exists.
      foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          set(false);
          return registry; // No mutation.
        }
      }

      // Okay, add the slave!
      Registry::Slave* slave = registry.mutable_slaves()->add_slaves();
      slave->mutable_info()->CopyFrom(info);
      return registry;
    }

    const SlaveInfo info;
  };

  // NOTE: Even though re-admission does not mutate the state, we
  // model it as a mutation so that it is performed in sequence with
  // other mutations.
  struct Readmit : Mutation<Registry>
  {
    Readmit(const SlaveInfo& _info) : info(_info) {}

    virtual Try<Registry> apply(Registry registry)
    {
      foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          set(true);
          return registry;
        }
      }
      set(false);
      return registry;
    }

    const SlaveInfo info;
  };

  struct Remove : Mutation<Registry>
  {
    Remove(const SlaveInfo& _info) : info(_info) {}

    virtual Try<Registry> apply(Registry registry)
    {
      for (int i = 0; i < registry.slaves().slaves().size(); i++) {
        const Registry::Slave& slave = registry.slaves().slaves(i);
        if (slave.info().id() == info.id()) {
          for (int j = i + 1; j < registry.slaves().slaves().size(); j++) {
            registry.mutable_slaves()->mutable_slaves()->SwapElements(j - 1, j);
          }
          registry.mutable_slaves()->mutable_slaves()->RemoveLast();
          return registry;
        }
      }

      set(false);
      return registry;
    }

    const SlaveInfo info;
  };

  Option<Variable<Registry> > variable;
  deque<Mutation<Registry>*> mutations;
  bool updating; // Used to signify fetching (recovering) or storing.

  // Continuations.
  Future<bool> _admit(const SlaveInfo& info);
  Future<bool> _readmit(const SlaveInfo& info);
  Future<bool> _remove(const SlaveInfo& info);

  // Helper for recovering state (performing fetch).
  Future<Nothing> recover();
  void _recover(const Future<Variable<Registry> >& recovery);

  // Helper for updating state (performing store).
  void update();
  Future<bool> _update(const Option<Variable<Registry> >& variable);
  void __update(const string& message);

  State* state;

  // Used to compose our operations with recovery.
  Promise<Nothing> recovered;
};


Future<Nothing> RegistrarProcess::recover()
{
  LOG(INFO) << "Recovering registrar";

  if (variable.isNone() && !updating) {
    // TODO(benh): Don't wait forever to recover?
    state->fetch<Registry>("registry")
      .onAny(defer(self(), &Self::_recover, lambda::_1));
    updating = true;
  }

  return recovered.future();
}


void RegistrarProcess::_recover(
    const Future<Variable<Registry> >& recovery)
{
  updating = false;

  CHECK(!recovery.isPending());

  if (recovery.isFailed() || recovery.isDiscarded()) {
    LOG(WARNING) << "Failed to recover registrar: "
                 << (recovery.isFailed() ? recovery.failure() : "discarded");
    recover(); // Retry! TODO(benh): Don't retry forever?
  } else {
    LOG(INFO) << "Successfully recovered registrar";

    // Save the registry.
    variable = recovery.get();

    // Signal the recovery is complete.
    recovered.set(Nothing());
  }
}


Future<bool> RegistrarProcess::admit(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_admit, info));
}


Future<bool> RegistrarProcess::_admit(const SlaveInfo& info)
{
  CHECK_SOME(variable);
  Mutation<Registry>* mutation = new Admit(info);
  mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::readmit(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_readmit, info));
}


Future<bool> RegistrarProcess::_readmit(
    const SlaveInfo& info)
{
  CHECK_SOME(variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Mutation<Registry>* mutation = new Readmit(info);
  mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::remove(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_remove, info));
}


Future<bool> RegistrarProcess::_remove(
    const SlaveInfo& info)
{
  CHECK_SOME(variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Mutation<Registry>* mutation = new Remove(info);
  mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!updating) {
    update();
  }
  return future;
}


void RegistrarProcess::update()
{
  if (mutations.empty()) {
    return; // No-op.
  }

  CHECK(!updating);

  updating = true;

  LOG(INFO) << "Attempting to update 'registry'";

  CHECK_SOME(variable);

  Variable<Registry> variable_ = variable.get();

  foreach (Mutation<Registry>* mutation, mutations) {
    Try<Registry> registry = mutation->apply(variable_.get());
    if (registry.isError()) {
      mutation->fail("Failed to mutate 'registry': " + registry.error());
    } else {
      variable_ = variable_.mutate(registry.get());
    }
  }

  // Perform the store! Save the future so we can associate it with
  // the mutations that are part of this update.
  Future<bool> future = state->store(variable_)
    .then(defer(self(), &Self::_update, lambda::_1));

  // TODO(benh): Add a timeout so we don't wait forever.

  // Toggle 'updating' if the store fails or is discarded.
  future
    .onDiscarded(defer(self(), &Self::__update, "discarded"))
    .onFailed(defer(self(), &Self::__update, lambda::_1));

  // Now associate the store with all the mutations.
  while (!mutations.empty()) {
    Mutation<Registry>* mutation = mutations.front();
    mutations.pop_front();
    mutation->associate(future); // No-op if already failed above.
    delete mutation;
  }
}


Future<bool> RegistrarProcess::_update(
    const Option<Variable<Registry> >& variable_)
{
  updating = false;

  if (variable_.isNone()) {
    LOG(WARNING) << "Failed to update 'registry': version mismatch";
    return Failure("Failed to update 'registry': version mismatch");
  }

  LOG(INFO) << "Successfully updated 'registry'";

  variable = variable_.get();

  if (!mutations.empty()) {
    update();
  }

  return true;
}


void RegistrarProcess::__update(const string& message)
{
  LOG(WARNING) << "Failed to update 'registry': " << message;
  updating = false;
}


Registrar::Registrar(State* state)
{
  process = new RegistrarProcess(state);
  spawn(process);
}


Registrar::~Registrar()
{
  terminate(process);
  wait(process);
}


Future<bool> Registrar::admit(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::admit, info);
}


Future<bool> Registrar::readmit(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::readmit, info);
}


Future<bool> Registrar::remove(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::remove, info);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
