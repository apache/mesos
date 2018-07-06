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

#include "resource_provider/registrar.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>

#include <glog/logging.h>

#include <mesos/type_utils.hpp>

#include <mesos/state/in_memory.hpp>

#include <mesos/state/protobuf.hpp>

#include <process/defer.hpp>
#include <process/process.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/unreachable.hpp>

#include "master/registrar.hpp"

#include "slave/paths.hpp"


using std::deque;
using std::string;

using mesos::resource_provider::registry::Registry;
using mesos::resource_provider::registry::ResourceProvider;

using mesos::state::Storage;

using mesos::state::protobuf::Variable;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;
using process::defer;
using process::spawn;
using process::terminate;
using process::wait;


namespace master = mesos::internal::master;
namespace slave = mesos::internal::slave;

namespace mesos {
namespace resource_provider {

Try<bool> Registrar::Operation::operator()(Registry* registry)
{
  Try<bool> result = perform(registry);

  success = !result.isError();

  return result;
}


bool Registrar::Operation::set()
{
  return Promise<bool>::set(success);
}


Try<Owned<Registrar>> Registrar::create(Owned<Storage> storage)
{
  return new GenericRegistrar(std::move(storage));
}


Try<Owned<Registrar>> Registrar::create(
    master::Registrar* registrar,
    Registry registry)
{
  return new MasterRegistrar(registrar, std::move(registry));
}


AdmitResourceProvider::AdmitResourceProvider(
    const ResourceProvider& _resourceProvider)
  : resourceProvider(_resourceProvider) {}


Try<bool> AdmitResourceProvider::perform(Registry* registry)
{
  if (std::find_if(
          registry->resource_providers().begin(),
          registry->resource_providers().end(),
          [this](const ResourceProvider& resourceProvider) {
            return resourceProvider.id() == this->resourceProvider.id();
          }) != registry->resource_providers().end()) {
    return Error("Resource provider already admitted");
  }

  if (std::find_if(
          registry->removed_resource_providers().begin(),
          registry->removed_resource_providers().end(),
          [this](const ResourceProvider& resourceProvider) {
            return resourceProvider.id() == this->resourceProvider.id();
          }) != registry->removed_resource_providers().end()) {
    return Error("Resource provider was removed");
  }

  registry->add_resource_providers()->CopyFrom(resourceProvider);

  return true; // Mutation.
}


RemoveResourceProvider::RemoveResourceProvider(const ResourceProviderID& _id)
  : id(_id) {}


Try<bool> RemoveResourceProvider::perform(Registry* registry)
{
  auto pos = std::find_if(
      registry->resource_providers().begin(),
      registry->resource_providers().end(),
      [this](const ResourceProvider& resourceProvider) {
        return resourceProvider.id() == this->id;
      });

  if (pos == registry->resource_providers().end()) {
    return Error("Attempted to remove an unknown resource provider");
  }

  registry->add_removed_resource_providers()->CopyFrom(*pos);
  registry->mutable_resource_providers()->erase(pos);

  return true; // Mutation.
}


class GenericRegistrarProcess : public Process<GenericRegistrarProcess>
{
public:
  GenericRegistrarProcess(Owned<Storage> storage);

  Future<Registry> recover();

  Future<bool> apply(Owned<Registrar::Operation> operation);

  void update();

  void initialize() override;

private:
  Future<bool> _apply(Owned<Registrar::Operation> operation);

  void _update(
      const Future<Option<Variable<Registry>>>& store,
      deque<Owned<Registrar::Operation>> applied);

  // We explicitly hold the storage to keep it alive over the
  // registrar's lifetime.
  Owned<Storage> storage;

  // Use fully qualified type for `State` to disambiguate with `State`
  // enumeration in `ProcessBase`.
  mesos::state::protobuf::State state;

  Promise<Nothing> recovered;
  Option<Variable<Registry>> variable;
  Option<Error> error;
  deque<Owned<Registrar::Operation>> operations;
  bool updating = false;
};


GenericRegistrarProcess::GenericRegistrarProcess(Owned<Storage> _storage)
  : ProcessBase(process::ID::generate("resource-provider-generic-registrar")),
    storage(std::move(_storage)),
    state(storage.get())
{
  CHECK_NOTNULL(storage.get());
}


void GenericRegistrarProcess::initialize()
{
  constexpr char NAME[] = "RESOURCE_PROVIDER_REGISTRAR";

  CHECK_NONE(variable);

  recovered.associate(state.fetch<Registry>(NAME).then(
      defer(self(), [this](const Variable<Registry>& recovery) {
        variable = recovery;
        return Nothing();
      })));
}


Future<Registry> GenericRegistrarProcess::recover()
{
  // Prevent discards on the returned `Future` by marking the result as
  // `undiscardable` so that we control the lifetime of the recovering registry.
  return undiscardable(recovered.future())
    .then(defer(self(), [this](const Nothing&) {
      CHECK_SOME(this->variable);
      return this->variable->get();
    }));
}


Future<bool> GenericRegistrarProcess::apply(
    Owned<Registrar::Operation> operation)
{
  return undiscardable(recovered.future()).then(
      defer(self(), &Self::_apply, std::move(operation)));
}


Future<bool> GenericRegistrarProcess::_apply(
    Owned<Registrar::Operation> operation)
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  operations.push_back(std::move(operation));

  Future<bool> future = operations.back()->future();
  if (!updating) {
    update();
  }

  return future;
}


void GenericRegistrarProcess::update()
{
  CHECK(!updating);
  CHECK_NONE(error);

  if (operations.empty()) {
    return; // No-op.
  }

  updating = true;

  CHECK_SOME(variable);

  Registry updatedRegistry = variable->get();

  foreach (Owned<Registrar::Operation>& operation, operations) {
    Try<bool> operationResult = (*operation)(&updatedRegistry);

    if (operationResult.isError()) {
      LOG(WARNING)
        << "Failed to apply operation on resource provider manager registry: "
        << operationResult.error();
    }
  }

  // Serialize updated registry.
  CHECK_SOME(variable);

  Future<Option<Variable<Registry>>> store =
    state.store(variable->mutate(updatedRegistry));

  store.onAny(defer(
      self(),
      &Self::_update,
      lambda::_1,
      std::move(operations)));

  operations.clear();
}


void GenericRegistrarProcess::_update(
    const Future<Option<Variable<Registry>>>& store,
    deque<Owned<Registrar::Operation>> applied)
{
  updating = false;
  // Abort if the storage operation did not succeed.
  if (!store.isReady() || store->isNone()) {
    string message = "Failed to update registry: ";

    if (store.isFailed()) {
      message += store.failure();
    } else if (store.isDiscarded()) {
      message += "discarded";
    } else {
      message += "version mismatch";
    }

    while (!applied.empty()) {
      applied.front()->fail(message);
      applied.pop_front();
    }

    error = Error(message);

    LOG(ERROR) << "Registrar aborting: " << message;

    return;
  }

  variable = store->get();

  // Remove the operations.
  while (!applied.empty()) {
    Owned<Registrar::Operation> operation = applied.front();
    applied.pop_front();

    operation->set();
  }

  if (!operations.empty()) {
    update();
  }
}


GenericRegistrar::GenericRegistrar(Owned<Storage> storage)
  : process(new GenericRegistrarProcess(std::move(storage)))
{
  process::spawn(process.get(), false);
}


GenericRegistrar::~GenericRegistrar()
{
  process::terminate(*process);
  process::wait(*process);
}


Future<Registry> GenericRegistrar::recover()
{
  return dispatch(process.get(), &GenericRegistrarProcess::recover);
}


Future<bool> GenericRegistrar::apply(Owned<Operation> operation)
{
  return dispatch(
      process.get(),
      &GenericRegistrarProcess::apply,
      std::move(operation));
}


class MasterRegistrarProcess : public Process<MasterRegistrarProcess>
{
  // A helper class for adapting operations on the resource provider
  // registry to the master registry.
  class AdaptedOperation : public master::RegistryOperation
  {
  public:
    AdaptedOperation(Owned<Registrar::Operation> operation);

    Future<registry::Registry> recover();

  private:
    Try<bool> perform(internal::Registry* registry, hashset<SlaveID>*) override;

    Owned<Registrar::Operation> operation;

    AdaptedOperation(const AdaptedOperation&) = delete;
    AdaptedOperation(AdaptedOperation&&) = default;
    AdaptedOperation& operator=(const AdaptedOperation&) = delete;
    AdaptedOperation& operator=(AdaptedOperation&&) = default;
  };

public:
  explicit MasterRegistrarProcess(
      master::Registrar* registrar,
      Registry registry);

  Future<bool> apply(Owned<Registrar::Operation> operation);

  Future<registry::Registry> recover() { return registry; }

private:
  master::Registrar* registrar = nullptr;
  Registry registry;
};


MasterRegistrarProcess::AdaptedOperation::AdaptedOperation(
    Owned<Registrar::Operation> operation)
  : operation(std::move(operation)) {}


Try<bool> MasterRegistrarProcess::AdaptedOperation::perform(
    internal::Registry* registry,
    hashset<SlaveID>*)
{
  return (*operation)(registry->mutable_resource_provider_registry());
}


MasterRegistrarProcess::MasterRegistrarProcess(
    master::Registrar* _registrar,
    registry::Registry _registry)
  : ProcessBase(process::ID::generate("resource-provider-agent-registrar")),
    registrar(_registrar),
    registry(std::move(_registry)) {}


Future<bool> MasterRegistrarProcess::apply(
    Owned<Registrar::Operation> operation)
{
  auto adaptedOperation = Owned<master::RegistryOperation>(
      new AdaptedOperation(std::move(operation)));

  return registrar->apply(std::move(adaptedOperation));
}


MasterRegistrar::MasterRegistrar(
    master::Registrar* registrar,
    registry::Registry registry)
  : process(new MasterRegistrarProcess(registrar, std::move(registry)))
{
  spawn(process.get(), false);
}


MasterRegistrar::~MasterRegistrar()
{
  terminate(*process);
  wait(*process);
}


Future<Registry> MasterRegistrar::recover()
{
  return dispatch(process.get(), &MasterRegistrarProcess::recover);
}


Future<bool> MasterRegistrar::apply(Owned<Operation> operation)
{
  return dispatch(
      process.get(),
      &MasterRegistrarProcess::apply,
      std::move(operation));
}

} // namespace resource_provider {
} // namespace mesos {
