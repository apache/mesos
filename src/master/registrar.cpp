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
#include <process/owned.hpp>
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
using process::Owned;
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

// TODO(bmahler): Consider an implementation that pushes the
// operations to the caller to simplify the interface:
//
// In this design, operations can be maintained in a separate
// header and added independently of the Registrar logic. However,
// we would need to ensure that all Operations can be generalized
// to result in a Future<bool>.
//
// Registrar
// {
//   Future<Registrar> recover(const MasterInfo&);
//   Future<bool> apply(const Operation&);
// }
//
// Registrar registrar(flags, state);
//
// Future<bool> admit = registrar.apply(Admit(slaveInfo));
// Future<bool> readmit = registrar.apply(Redmit(slaveInfo));
// Future<bool> remove = registrar.apply(Remove(slaveInfo));

class RegistrarProcess : public Process<RegistrarProcess>
{
public:
  RegistrarProcess(const Flags& _flags, State* _state)
    : ProcessBase("registrar"),
      updating(false),
      flags(_flags),
      state(_state) {}

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<Registry> recover(const MasterInfo& info);
  Future<bool> admit(const SlaveInfo& info);
  Future<bool> readmit(const SlaveInfo& info);
  Future<bool> remove(const SlaveInfo& info);

private:
  template <typename T>
  struct Operation : process::Promise<bool>
  {
    Operation() : success(false) {}

    // Attempts to invoke the operation on 't'.
    // Returns whether the operation mutates 't', or an error if the
    // operation cannot be applied successfully.
    Try<bool> operator () (T* t, bool strict)
    {
      const Try<bool>& result = perform(t, strict);

      success = !result.isError();

      return result;
    }

    // Sets the promise based on whether the operation was successful.
    bool set() { return Promise<bool>::set(success); }

  protected:
    virtual Try<bool> perform(T* t, bool strict) = 0;

  private:
    bool success;
  };

  // The 'Recover' operation adds the latest MasterInfo.
  struct Recover : Operation<Registry>
  {
    Recover(const MasterInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(Registry* registry, bool strict)
    {
      registry->mutable_master()->mutable_info()->CopyFrom(info);
      return true;
    }

    const MasterInfo info;
  };

  // Slave Admission.
  struct Admit : Operation<Registry>
  {
    Admit(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(Registry* registry, bool strict)
    {
      // Check and see if this slave already exists.
      foreach (const Registry::Slave& slave, registry->slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          if (strict) {
            return Error("Slave already admitted");
          } else {
            return false; // No mutation.
          }
        }
      }

      Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
      slave->mutable_info()->CopyFrom(info);
      return true; // Mutation.
    }

    const SlaveInfo info;
  };

  // Slave Readmission.
  struct Readmit : Operation<Registry>
  {
    Readmit(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(Registry* registry, bool strict)
    {
      foreach (const Registry::Slave& slave, registry->slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          return false; // No mutation.
        }
      }

      if (strict) {
        return Error("Slave not yet admitted");
      } else {
        Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
        slave->mutable_info()->CopyFrom(info);
        return true; // Mutation.
      }
    }

    const SlaveInfo info;
  };

  // Slave Removal.
  struct Remove : Operation<Registry>
  {
    Remove(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(Registry* registry, bool strict)
    {
      for (int i = 0; i < registry->slaves().slaves().size(); i++) {
        const Registry::Slave& slave = registry->slaves().slaves(i);
        if (slave.info().id() == info.id()) {
          for (int j = i + 1; j < registry->slaves().slaves().size(); j++) {
            registry->
                mutable_slaves()->mutable_slaves()->SwapElements(j - 1, j);
          }
          registry->mutable_slaves()->mutable_slaves()->RemoveLast();
          return true;
        }
      }

      if (strict) {
        return Error("Slave not yet admitted");
      } else {
        return false; // No mutation.
      }
    }

    const SlaveInfo info;
  };

  Option<Variable<Registry> > variable;
  deque<Operation<Registry>*> operations;
  bool updating; // Used to signify fetching (recovering) or storing.

  // Continuations.
  void _recover(
      const MasterInfo& info,
      const Future<Variable<Registry> >& recovery);
  void __recover(const Future<bool>& recover);
  Future<bool> _admit(const SlaveInfo& info);
  Future<bool> _readmit(const SlaveInfo& info);
  Future<bool> _remove(const SlaveInfo& info);

  // Helper for updating state (performing store).
  void update();
  void _update(
      const Future<Option<Variable<Registry> > >& store,
      deque<Operation<Registry>*> operations);

  const Flags flags;
  State* state;

  // Used to compose our operations with recovery.
  Option<Owned<Promise<Registry> > > recovered;
};


Future<Registry> RegistrarProcess::recover(const MasterInfo& info)
{
  LOG(INFO) << "Recovering registrar";

  if (recovered.isNone()) {
    // TODO(benh): Don't wait forever to recover?
    state->fetch<Registry>("registry")
      .onAny(defer(self(), &Self::_recover, info, lambda::_1));
    updating = true;
    recovered = Owned<Promise<Registry> >(new Promise<Registry>());
  }

  return recovered.get()->future();
}


void RegistrarProcess::_recover(
    const MasterInfo& info,
    const Future<Variable<Registry> >& recovery)
{
  updating = false;

  CHECK(!recovery.isPending());

  if (!recovery.isReady()) {
    recovered.get()->fail("Failed to recover registrar: " +
        (recovery.isFailed() ? recovery.failure() : "discarded"));
  } else {
    LOG(INFO) << "Successfully recovered registrar";

    // Save the registry.
    variable = recovery.get();

    // Perform the Recover operation to add the new MasterInfo.
    Operation<Registry>* operation = new Recover(info);
    operations.push_back(operation);
    operation->future()
      .onAny(defer(self(), &Self::__recover, lambda::_1));

    update();
  }
}


void RegistrarProcess::__recover(const Future<bool>& recover)
{
  CHECK(!recover.isPending());

  if (!recover.isReady()) {
    recovered.get()->fail("Failed to recover registrar: "
        "Failed to persist MasterInfo: " +
        (recover.isFailed() ? recover.failure() : "discarded"));
  } else if (!recover.get()) {
    recovered.get()->fail("Failed to recover registrar: "
        "Failed to persist MasterInfo: version mismatch");
  } else {
    // At this point _update() has updated 'variable' to contain
    // the Registry with the latest MasterInfo.
    // Set the promise and un-gate any pending operations.
    CHECK_SOME(variable);
    recovered.get()->set(variable.get().get());
  }
}


Future<bool> RegistrarProcess::admit(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  if (recovered.isNone()) {
    return Failure("Attempted to admit slave before recovering");
  }

  return recovered.get()->future()
    .then(defer(self(), &Self::_admit, info));
}


Future<bool> RegistrarProcess::_admit(const SlaveInfo& info)
{
  CHECK_SOME(variable);

  Operation<Registry>* operation = new Admit(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
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

  if (recovered.isNone()) {
    return Failure("Attempted to readmit slave before recovering");
  }

  return recovered.get()->future()
    .then(defer(self(), &Self::_readmit, info));
}


Future<bool> RegistrarProcess::_readmit(const SlaveInfo& info)
{
  CHECK_SOME(variable);

  Operation<Registry>* operation = new Readmit(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
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

  if (recovered.isNone()) {
    return Failure("Attempted to remove slave before recovering");
  }

  return recovered.get()->future()
    .then(defer(self(), &Self::_remove, info));
}


Future<bool> RegistrarProcess::_remove(const SlaveInfo& info)
{
  CHECK_SOME(variable);

  Operation<Registry>* operation = new Remove(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
  if (!updating) {
    update();
  }
  return future;
}


void RegistrarProcess::update()
{
  if (operations.empty()) {
    return; // No-op.
  }

  CHECK(!updating);

  updating = true;

  LOG(INFO) << "Attempting to update the 'registry'";

  CHECK_SOME(variable);

  Registry registry = variable.get().get();

  foreach (Operation<Registry>* operation, operations) {
    // No need to process the result of the operation.
    (*operation)(&registry, flags.registry_strict);
  }

  // TODO(benh): Add a timeout so we don't wait forever.

  // Perform the store!
  state->store(variable.get().mutate(registry))
    .onAny(defer(self(), &Self::_update, lambda::_1, operations));

  // Clear the operations, _update will transition the Promises!
  operations.clear();
}


void RegistrarProcess::_update(
    const Future<Option<Variable<Registry> > >& store,
    deque<Operation<Registry>*> operations)
{
  updating = false;

  // Set the variable if the storage operation succeeded.
  if (!store.isReady()) {
    LOG(ERROR) << "Failed to update 'registry': "
               << (store.isFailed() ? store.failure() : "discarded");
  } else if (store.get().isNone()) {
    LOG(WARNING) << "Failed to update 'registry': version mismatch";
  } else {
    LOG(INFO) << "Successfully updated 'registry'";
    variable = store.get().get();
  }

  // Remove the operations.
  while (!operations.empty()) {
    Operation<Registry>* operation = operations.front();
    operations.pop_front();

    if (!store.isReady()) {
      operation->fail("Failed to update 'registry': " +
          (store.isFailed() ? store.failure() : "discarded"));
    } else {
      if (store.get().isNone()) {
        operation->fail("Failed to update 'registry': version mismatch");
      } else {
        operation->set();
      }
    }

    delete operation;
  }

  operations.clear();

  if (!this->operations.empty()) {
    update();
  }
}


Registrar::Registrar(const Flags& flags, State* state)
{
  process = new RegistrarProcess(flags, state);
  spawn(process);
}


Registrar::~Registrar()
{
  terminate(process);
  wait(process);
}


Future<Registry> Registrar::recover(const MasterInfo& info)
{
  return dispatch(process, &RegistrarProcess::recover, info);
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
