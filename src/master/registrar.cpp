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

#include <deque>
#include <string>

#include <mesos/type_utils.hpp>

#include <mesos/state/state.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/timer.hpp>

#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/stopwatch.hpp>

#include "master/registrar.hpp"
#include "master/registry.hpp"

using mesos::state::State;
using mesos::state::Variable;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait; // Necessary on some OS's to disambiguate.

using process::AUTHENTICATION;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::TLDR;

using process::http::OK;

using process::http::authentication::Principal;

using process::metrics::PullGauge;
using process::metrics::Timer;

using std::deque;
using std::string;

namespace mesos {
namespace internal {
namespace master {

using process::http::Response;
using process::http::Request;

class RegistrarProcess : public Process<RegistrarProcess>
{
public:
  using State = mesos::state::State; // `ProcessBase::State` conflicts here.

  RegistrarProcess(
      const Flags& _flags,
      State* _state,
      const Option<string>& _authenticationRealm)
    : ProcessBase(process::ID::generate("registrar")),
      metrics(*this),
      state(_state),
      updating(false),
      flags(_flags),
      authenticationRealm(_authenticationRealm) {}

  ~RegistrarProcess() override {}

  // Registrar implementation.
  Future<Registry> recover(const MasterInfo& info);
  Future<bool> apply(Owned<RegistryOperation> operation);

protected:
  void initialize() override
  {
      route(
          "/registry",
          authenticationRealm,
          registryHelp(),
          &RegistrarProcess::getRegistry);
  }

private:
  // HTTP handlers.
  // /registrar(N)/registry
  Future<Response> getRegistry(
      const Request& request,
      const Option<Principal>&);
  static string registryHelp();

  // The 'Recover' operation adds the latest MasterInfo.
  class Recover : public RegistryOperation
  {
  public:
    explicit Recover(const MasterInfo& _info) : info(_info) {}

  protected:
    Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override
    {
      registry->mutable_master()->mutable_info()->CopyFrom(info);
      return true; // Mutation.
    }

  private:
    const MasterInfo info;
  };

  // Metrics.
  struct Metrics
  {
    explicit Metrics(const RegistrarProcess& process)
      : queued_operations(
            "registrar/queued_operations",
            defer(process, &RegistrarProcess::_queued_operations)),
        registry_size_bytes(
            "registrar/registry_size_bytes",
            defer(process, &RegistrarProcess::_registry_size_bytes)),
        state_fetch("registrar/state_fetch"),
        state_store("registrar/state_store", Days(1))
    {
      process::metrics::add(queued_operations);
      process::metrics::add(registry_size_bytes);

      process::metrics::add(state_fetch);
      process::metrics::add(state_store);
    }

    ~Metrics()
    {
      process::metrics::remove(queued_operations);
      process::metrics::remove(registry_size_bytes);

      process::metrics::remove(state_fetch);
      process::metrics::remove(state_store);
    }

    PullGauge queued_operations;
    PullGauge registry_size_bytes;

    Timer<Milliseconds> state_fetch;
    Timer<Milliseconds> state_store;
  } metrics;

  // PullGauge handlers.
  double _queued_operations()
  {
    return static_cast<double>(operations.size());
  }

  Future<double> _registry_size_bytes()
  {
    if (registry.isSome()) {
      return registry->ByteSize();
    }

    return Failure("Not recovered yet");
  }

  // Continuations.
  void _recover(
      const MasterInfo& info,
      const Future<Variable>& recovery);
  void __recover(const Future<bool>& recover);
  Future<bool> _apply(Owned<RegistryOperation> operation);

  // Helper for updating state (performing store).
  void update();
  void _update(
      const Future<Option<Variable>>& store,
      const Owned<Registry>& updatedRegistry,
      deque<Owned<RegistryOperation>> operations);

  // Fails all pending operations and transitions the Registrar
  // into an error state in which all subsequent operations will fail.
  // This ensures we don't attempt to re-acquire log leadership by
  // performing more State storage operations.
  void abort(const string& message);

  // TODO(ipronin): We use the "untyped" `State` class here and perform
  // the protobuf (de)serialization manually within the Registrar, because
  // the use of `protobuf::State` incurs a dramatic peformance cost from
  // protobuf copying. We should explore using `protobuf::State`, which will
  // require move support and other copy elimination to maintain the
  // performance of the current approach.
  State* state;

  // Per the TODO above, we store both serialized and deserialized versions
  // of the `Registry` protobuf. If we're able to move to `protobuf::State`,
  // we could just store a single `protobuf::state::Variable<Registry>`.
  Option<Variable> variable;
  Option<Registry> registry;

  deque<Owned<RegistryOperation>> operations;
  bool updating; // Used to signify fetching (recovering) or storing.

  const Flags flags;

  // Used to compose our operations with recovery.
  Option<Owned<Promise<Registry>>> recovered;

  // When an error is encountered from abort(), we'll fail all
  // subsequent operations.
  Option<Error> error;

  // The authentication realm, if any, into which this process'
  // endpoints will be installed.
  Option<string> authenticationRealm;
};


// Helper for treating State operations that timeout as failures.
template <typename T>
Future<T> timeout(
    const string& operation,
    const Duration& duration,
    Future<T> future)
{
  future.discard();

  return Failure(
      "Failed to perform " + operation + " within " + stringify(duration));
}


// Helper for failing a deque of operations.
void fail(deque<Owned<RegistryOperation>>* operations, const string& message)
{
  while (!operations->empty()) {
    operations->front()->fail(message);
    operations->pop_front();
  }
}


Future<Response> RegistrarProcess::getRegistry(
    const Request& request,
    const Option<Principal>&)
{
  JSON::Object result;

  if (registry.isSome()) {
    result = JSON::protobuf(registry.get());
  }

  return OK(result, request.url.query.get("jsonp"));
}


string RegistrarProcess::registryHelp()
{
  return HELP(
      TLDR(
          "Returns the current contents of the Registry in JSON."),
      DESCRIPTION(
          "Example:",
          "",
          "```",
          "{",
          "  \"master\":",
          "  {",
          "    \"info\":",
          "    {",
          "      \"hostname\": \"localhost\",",
          "      \"id\": \"20140325-235542-1740121354-5050-33357\",",
          "      \"ip\": 2130706433,",
          "      \"pid\": \"master@127.0.0.1:5050\",",
          "      \"port\": 5050",
          "    }",
          "  },",
          "",
          "  \"slaves\":",
          "  {",
          "    \"slaves\":",
          "    [",
          "      {",
          "        \"info\":",
          "        {",
          "          \"checkpoint\": true,",
          "          \"hostname\": \"localhost\",",
          "          \"id\":",
          "          {",
          "            \"value\": \"20140325-234618-1740121354-5050-29065-0\"",
          "          },",
          "          \"port\": 5051,",
          "          \"resources\":",
          "          [",
          "            {",
          "              \"name\": \"cpus\",",
          "              \"role\": \"*\",",
          "              \"scalar\": { \"value\": 24 },",
          "              \"type\": \"SCALAR\"",
          "            }",
          "          ],",
          "        }",
          "      }",
          "    ]",
          "  }",
          "}",
          "```"),
      AUTHENTICATION(true));
}


Future<Registry> RegistrarProcess::recover(const MasterInfo& info)
{
  if (recovered.isNone()) {
    VLOG(1) << "Recovering registrar";

    metrics.state_fetch.start();
    state->fetch("registry")
      .after(flags.registry_fetch_timeout,
             lambda::bind(
                 &timeout<Variable>,
                 "fetch",
                 flags.registry_fetch_timeout,
                 lambda::_1))
      .onAny(defer(self(), &Self::_recover, info, lambda::_1));
    updating = true;
    recovered = Owned<Promise<Registry>>(new Promise<Registry>());
  }

  return recovered.get()->future();
}


void RegistrarProcess::_recover(
    const MasterInfo& info,
    const Future<Variable>& recovery)
{
  updating = false;

  CHECK(!recovery.isPending());

  if (!recovery.isReady()) {
    recovered.get()->fail("Failed to recover registrar: " +
        (recovery.isFailed() ? recovery.failure() : "discarded"));
    return;
  }

  // Deserialize the registry.
  Try<Registry> deserialized =
    ::protobuf::deserialize<Registry>(recovery->value());
  if (deserialized.isError()) {
    recovered.get()->fail("Failed to recover registrar: " +
                          deserialized.error());
    return;
  }

  Duration elapsed = metrics.state_fetch.stop();

  LOG(INFO) << "Successfully fetched the registry"
            << " (" << Bytes(deserialized->ByteSize()) << ")"
            << " in " << elapsed;

  // Save the registry.
  variable = recovery.get();

  // Workaround for immovable protobuf messages.
  registry = Option<Registry>(Registry());
  registry->Swap(&deserialized.get());

  // Perform the Recover operation to add the new MasterInfo.
  Owned<RegistryOperation> operation(new Recover(info));
  operations.push_back(operation);
  operation->future()
    .onAny(defer(self(), &Self::__recover, lambda::_1));

  update();
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
    LOG(INFO) << "Successfully recovered registrar";

    // At this point _update() has updated 'variable' to contain
    // the Registry with the latest MasterInfo.
    // Set the promise and un-gate any pending operations.
    CHECK_SOME(variable);
    CHECK_SOME(registry);
    recovered.get()->set(registry.get());
  }
}


Future<bool> RegistrarProcess::apply(Owned<RegistryOperation> operation)
{
  if (recovered.isNone()) {
    return Failure("Attempted to apply the operation before recovering");
  }

  return recovered.get()->future()
    .then(defer(self(), &Self::_apply, operation));
}


Future<bool> RegistrarProcess::_apply(Owned<RegistryOperation> operation)
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  CHECK_SOME(variable);

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
  CHECK_NONE(error);
  CHECK_SOME(variable);

  // Time how long it takes to apply the operations.
  Stopwatch stopwatch;
  stopwatch.start();

  updating = true;

  // Create a snapshot of the current registry. We use an `Owned` here
  // to avoid copying, since protobuf doesn't suppport move construction.
  auto updatedRegistry = Owned<Registry>(new Registry(registry.get()));

  // Create the 'slaveIDs' accumulator.
  hashset<SlaveID> slaveIDs;
  foreach (const Registry::Slave& slave, updatedRegistry->slaves().slaves()) {
    slaveIDs.insert(slave.info().id());
  }

  foreach (Owned<RegistryOperation>& operation, operations) {
    // No need to process the result of the operation.
    (*operation)(updatedRegistry.get(), &slaveIDs);
  }

  LOG(INFO) << "Applied " << operations.size() << " operations in "
            << stopwatch.elapsed() << "; attempting to update the registry";

  // Perform the store, and time the operation.
  metrics.state_store.start();

  // Serialize updated registry.
  Try<string> serialized = ::protobuf::serialize(*updatedRegistry);
  if (serialized.isError()) {
    string message = "Failed to update registry: " + serialized.error();
    fail(&operations, message);
    abort(message);
    return;
  }

  state->store(variable->mutate(serialized.get()))
    .after(flags.registry_store_timeout,
           lambda::bind(
               &timeout<Option<Variable>>,
               "store",
               flags.registry_store_timeout,
               lambda::_1))
    .onAny(defer(
        self(), &Self::_update, lambda::_1, updatedRegistry, operations));

  // Clear the operations, _update will transition the Promises!
  operations.clear();
}


void RegistrarProcess::_update(
    const Future<Option<Variable>>& store,
    const Owned<Registry>& updatedRegistry,
    deque<Owned<RegistryOperation>> applied)
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

    fail(&applied, message);
    abort(message);

    return;
  }

  Duration elapsed = metrics.state_store.stop();

  LOG(INFO) << "Successfully updated the registry in " << elapsed;

  variable = store->get();
  registry->Swap(updatedRegistry.get());

  // Remove the operations.
  while (!applied.empty()) {
    Owned<RegistryOperation> operation = applied.front();
    applied.pop_front();

    operation->set();
  }

  if (!operations.empty()) {
    update();
  }
}


void RegistrarProcess::abort(const string& message)
{
  error = Error(message);

  LOG(ERROR) << "Registrar aborting: " << message;

  fail(&operations, message);
}


Registrar::Registrar(
    const Flags& flags,
    State* state,
    const Option<string>& authenticationRealm)
{
  process = new RegistrarProcess(flags, state, authenticationRealm);
  spawn(process);
}


Registrar::~Registrar()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Registry> Registrar::recover(const MasterInfo& info)
{
  return dispatch(process, &RegistrarProcess::recover, info);
}


Future<bool> Registrar::apply(Owned<RegistryOperation> operation)
{
  return dispatch(process, &RegistrarProcess::apply, operation);
}


PID<RegistrarProcess> Registrar::pid() const
{
  return process->self();
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
