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

#include <mesos/state/protobuf.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <process/metrics/gauge.hpp>
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

using mesos::state::protobuf::State;
using mesos::state::protobuf::Variable;

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

using process::metrics::Gauge;
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
  RegistrarProcess(
      const Flags& _flags,
      State* _state,
      const Option<string>& _authenticationRealm)
    : ProcessBase(process::ID::generate("registrar")),
      metrics(*this),
      updating(false),
      flags(_flags),
      state(_state),
      authenticationRealm(_authenticationRealm) {}

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<Registry> recover(const MasterInfo& info);
  Future<bool> apply(Owned<Operation> operation);

protected:
  virtual void initialize()
  {
    if (authenticationRealm.isSome()) {
      route(
          "/registry",
          authenticationRealm.get(),
          registryHelp(),
          &RegistrarProcess::registry);
    } else {
      route(
          "/registry",
          registryHelp(),
          lambda::bind(&RegistrarProcess::registry, this, lambda::_1, None()));
    }
  }

private:
  // HTTP handlers.
  // /registrar(N)/registry
  Future<Response> registry(
      const Request& request,
      const Option<string>& /* principal */);
  static string registryHelp();

  // The 'Recover' operation adds the latest MasterInfo.
  class Recover : public Operation
  {
  public:
    explicit Recover(const MasterInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(
        Registry* registry,
        hashset<SlaveID>* slaveIDs,
        bool strict)
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

    Gauge queued_operations;
    Gauge registry_size_bytes;

    Timer<Milliseconds> state_fetch;
    Timer<Milliseconds> state_store;
  } metrics;

  // Gauge handlers.
  double _queued_operations()
  {
    return operations.size();
  }

  Future<double> _registry_size_bytes()
  {
    if (variable.isSome()) {
      return variable.get().get().ByteSize();
    }

    return Failure("Not recovered yet");
  }

  // Continuations.
  void _recover(
      const MasterInfo& info,
      const Future<Variable<Registry> >& recovery);
  void __recover(const Future<bool>& recover);
  Future<bool> _apply(Owned<Operation> operation);

  // Helper for updating state (performing store).
  void update();
  void _update(
      const Future<Option<Variable<Registry> > >& store,
      deque<Owned<Operation> > operations);

  // Fails all pending operations and transitions the Registrar
  // into an error state in which all subsequent operations will fail.
  // This ensures we don't attempt to re-acquire log leadership by
  // performing more State storage operations.
  void abort(const string& message);

  Option<Variable<Registry> > variable;
  deque<Owned<Operation> > operations;
  bool updating; // Used to signify fetching (recovering) or storing.

  const Flags flags;
  State* state;

  // Used to compose our operations with recovery.
  Option<Owned<Promise<Registry> > > recovered;

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
void fail(deque<Owned<Operation>>* operations, const string& message)
{
  while (!operations->empty()) {
    operations->front()->fail(message);
    operations->pop_front();
  }
}


Future<Response> RegistrarProcess::registry(
    const Request& request,
    const Option<string>& /* principal */)
{
  JSON::Object result;

  if (variable.isSome()) {
    result = JSON::protobuf(variable.get().get());
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
    LOG(INFO) << "Recovering registrar";

    metrics.state_fetch.start();
    state->fetch<Registry>("registry")
      .after(flags.registry_fetch_timeout,
             lambda::bind(
                 &timeout<Variable<Registry> >,
                 "fetch",
                 flags.registry_fetch_timeout,
                 lambda::_1))
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
    Duration elapsed = metrics.state_fetch.stop();

    LOG(INFO) << "Successfully fetched the registry"
              << " (" << Bytes(recovery.get().get().ByteSize()) << ")"
              << " in " << elapsed;

    // Save the registry.
    variable = recovery.get();

    // Perform the Recover operation to add the new MasterInfo.
    Owned<Operation> operation(new Recover(info));
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
    LOG(INFO) << "Successfully recovered registrar";

    // At this point _update() has updated 'variable' to contain
    // the Registry with the latest MasterInfo.
    // Set the promise and un-gate any pending operations.
    CHECK_SOME(variable);
    recovered.get()->set(variable.get().get());
  }
}


Future<bool> RegistrarProcess::apply(Owned<Operation> operation)
{
  if (recovered.isNone()) {
    return Failure("Attempted to apply the operation before recovering");
  }

  return recovered.get()->future()
    .then(defer(self(), &Self::_apply, operation));
}


Future<bool> RegistrarProcess::_apply(Owned<Operation> operation)
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

  // Create a snapshot of the current registry.
  Registry registry = variable.get().get();

  // Create the 'slaveIDs' accumulator.
  hashset<SlaveID> slaveIDs;
  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    slaveIDs.insert(slave.info().id());
  }

  foreach (Owned<Operation> operation, operations) {
    // No need to process the result of the operation.
    (*operation)(&registry, &slaveIDs, flags.registry_strict);
  }

  LOG(INFO) << "Applied " << operations.size() << " operations in "
            << stopwatch.elapsed() << "; attempting to update the 'registry'";

  // Perform the store, and time the operation.
  metrics.state_store.start();
  state->store(variable.get().mutate(registry))
    .after(flags.registry_store_timeout,
           lambda::bind(
               &timeout<Option<Variable<Registry> > >,
               "store",
               flags.registry_store_timeout,
               lambda::_1))
    .onAny(defer(self(), &Self::_update, lambda::_1, operations));

  // Clear the operations, _update will transition the Promises!
  operations.clear();
}


void RegistrarProcess::_update(
    const Future<Option<Variable<Registry> > >& store,
    deque<Owned<Operation> > applied)
{
  updating = false;

  // Abort if the storage operation did not succeed.
  if (!store.isReady() || store.get().isNone()) {
    string message = "Failed to update 'registry': ";

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

  LOG(INFO) << "Successfully updated the 'registry' in " << elapsed;

  variable = store.get().get();

  // Remove the operations.
  while (!applied.empty()) {
    Owned<Operation> operation = applied.front();
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


Future<bool> Registrar::apply(Owned<Operation> operation)
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
