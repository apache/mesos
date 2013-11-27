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

namespace mesos {
namespace internal {
namespace master {

class RegistrarProcess : public Process<RegistrarProcess>
{
public:
  RegistrarProcess(State* _state)
    : ProcessBase("registrar"),
      state(_state)
  {
    slaves.variable = None();
    slaves.updating = false;
  }

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<bool> admit(const SlaveID& id, const SlaveInfo& info);
  Future<bool> readmit(const SlaveInfo& info);
  Future<bool> remove(const SlaveInfo& info);

private:
  template <typename T>
  struct Mutation : process::Promise<bool>
  {
    virtual Try<T> apply(T t) = 0;
  };

  struct Admit : Mutation<registry::Slaves>
  {
    Admit(const SlaveID& _id, const SlaveInfo& _info)
      : id(_id), info(_info) {}

    virtual Try<registry::Slaves> apply(registry::Slaves slaves)
    {
      // Check and see if this slave already exists.
      foreach (const registry::Slave& slave, slaves.slaves()) {
        if (slave.info().id() == id) {
          set(false);
          return slaves; // No mutation.
        }
      }

      // Okay, add the slave!
      registry::Slave* slave = slaves.add_slaves();
      slave->mutable_info()->CopyFrom(info);
      slave->mutable_info()->mutable_id()->MergeFrom(id);
      return slaves;
    }

    const SlaveID id;
    const SlaveInfo info;
  };

  // NOTE: even thought readmission does not mutate the state we model
  // it as a mutation so that it is performed in sequence with other
  // mutations.
  struct Readmit : Mutation<registry::Slaves>
  {
    Readmit(const SlaveInfo& _info) : info(_info) { CHECK(info.has_id()); }

    virtual Try<registry::Slaves> apply(registry::Slaves slaves)
    {
      bool found = false;
      foreach (const registry::Slave& slave, slaves.slaves()) {
        if (slave.info().id() == info.id()) {
          set(true);
          found = true;
        }
      }
      if (!found) {
        set(false);
      }
      return slaves;
    }

    const SlaveInfo info;
  };

  struct Remove : Mutation<registry::Slaves>
  {
    Remove(const SlaveInfo& _info) : info(_info) { CHECK(info.has_id()); }

    virtual Try<registry::Slaves> apply(registry::Slaves slaves)
    {
      bool removed = false;
      for (int i = 0; i < slaves.slaves().size(); i++) {
        const registry::Slave& slave = slaves.slaves(i);
        if (slave.info().id() == info.id()) {
          for (int j = i + 1; j < slaves.slaves().size(); j++) {
            slaves.mutable_slaves()->SwapElements(i, j);
          }
          slaves.mutable_slaves()->RemoveLast();
          removed = true;
          break;
        }
      }
      if (!removed) {
        set(false);
      }
      return slaves; // May or may not have been mutated.
    }

    const SlaveInfo info;
  };

  struct {
    Option<Variable<registry::Slaves> > variable;
    std::deque<Mutation<registry::Slaves>*> mutations;
    bool updating; // Used to signify fetching (recovering) or storing.
  } slaves;

  // Continuations.
  Future<bool> _admit(const SlaveID& id, const SlaveInfo& info);
  Future<bool> _readmit(const SlaveInfo& info);
  Future<bool> _remove(const SlaveInfo& info);

  // Helper for recovering state (performing fetch).
  Future<Nothing> recover();
  void _recover(const Future<Variable<registry::Slaves> >& recovery);

  // Helper for updating state (performing store).
  void update();
  Future<bool> _update(const Option<Variable<registry::Slaves> >& variable);
  void __update();

  State* state;

  // Used to compose our operations with recovery.
  Promise<Nothing> recovered;
};


Future<Nothing> RegistrarProcess::recover()
{
  LOG(INFO) << "Recovering registrar";

  // "Recover" the 'slaves' variable by fetching it from the state.
  if (slaves.variable.isNone() && !slaves.updating) {
    state->fetch<registry::Slaves>("slaves")
      .onAny(defer(self(), &Self::_recover, lambda::_1));

    // TODO(benh): Don't wait forever to recover?
  }

  // TODO(benh): Recover other variables too.

  return recovered.future();
}


void RegistrarProcess::_recover(
    const Future<Variable<registry::Slaves> >& recovery)
{
  slaves.updating = false;

  CHECK(!recovery.isPending());

  if (recovery.isFailed() || recovery.isDiscarded()) {
    LOG(WARNING) << "Failed to recover registrar: " << recovery.isFailed()
      ? recovery.failure() : "future discarded";
    recover(); // Retry! TODO(benh): Don't retry forever?
  } else {
    LOG(INFO) << "Successfully recovered registrar";

    // Save the slaves variable.
    slaves.variable = recovery.get();

    // Signal the recovery is complete.
    recovered.set(Nothing());
  }
}


Future<bool> RegistrarProcess::admit(
    const SlaveID& id,
    const SlaveInfo& info)
{
  return recover()
    .then(defer(self(), &Self::_admit, id, info));
}


Future<bool> RegistrarProcess::_admit(
    const SlaveID& id,
    const SlaveInfo& info)
{
  CHECK_SOME(slaves.variable);
  Mutation<registry::Slaves>* mutation = new Admit(id, info);
  slaves.mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!slaves.updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::readmit(const SlaveInfo& info)
{
  return recover()
    .then(defer(self(), &Self::_readmit, info));
}


Future<bool> RegistrarProcess::_readmit(
    const SlaveInfo& info)
{
  CHECK_SOME(slaves.variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Mutation<registry::Slaves>* mutation = new Readmit(info);
  slaves.mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!slaves.updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::remove(const SlaveInfo& info)
{
  return recover()
    .then(defer(self(), &Self::_remove, info));
}


Future<bool> RegistrarProcess::_remove(
    const SlaveInfo& info)
{
  CHECK_SOME(slaves.variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Mutation<registry::Slaves>* mutation = new Remove(info);
  slaves.mutations.push_back(mutation);
  Future<bool> future = mutation->future();
  if (!slaves.updating) {
    update();
  }
  return future;
}


void RegistrarProcess::update()
{
  if (!slaves.mutations.empty()) {
    CHECK(!slaves.updating);

    slaves.updating = true;

    LOG(INFO) << "Attempting to update 'slaves'";

    CHECK_SOME(slaves.variable);

    Variable<registry::Slaves> variable = slaves.variable.get();

    foreach (Mutation<registry::Slaves>* mutation, slaves.mutations) {
      Try<registry::Slaves> slaves = mutation->apply(variable.get());
      if (slaves.isError()) {
        mutation->fail("Failed to mutate 'slaves': " + slaves.error());
      } else {
        Try<Variable<registry::Slaves> > v = variable.mutate(slaves.get());
        if (v.isError()) {
          mutation->fail("Failed to mutate 'slaves': " + v.error());
        } else {
          variable = v.get();
        }
      }
    }

    // Perform the store! Save the future so we can associate it with
    // the mutations that are part of this update.
    Future<bool> future =
      state->store(variable).then(defer(self(), &Self::_update, lambda::_1));

    // TODO(benh): Add a timeout so we don't wait forever.

    // Toggle 'updating' if the store fails or is discarded.
    future
      .onDiscarded(defer(self(), &Self::__update))
      .onFailed(defer(self(), &Self::__update));

    // Now associate the store with all the mutations.
    while (!slaves.mutations.empty()) {
      Mutation<registry::Slaves>* mutation = slaves.mutations.front();
      slaves.mutations.pop_front();
      mutation->associate(future); // No-op if already failed above.
      delete mutation;
    }
  }
}


Future<bool> RegistrarProcess::_update(
    const Option<Variable<registry::Slaves> >& variable)
{
  slaves.updating = false;

  if (variable.isNone()) {
    LOG(WARNING) << "Failed to update 'slaves': version mismatch";
    return Failure("Failed to update 'slaves': version mismatch");
  }

  LOG(INFO) << "Successfully updated 'slaves'";

  slaves.variable = variable.get();

  if (!slaves.mutations.empty()) {
    update();
  }

  return true;
}


void RegistrarProcess::__update()
{
  LOG(WARNING) << "Failed to update 'slaves'";
  slaves.updating = false;
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


Future<bool> Registrar::admit(
    const SlaveID& id,
    const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::admit, id, info);
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
