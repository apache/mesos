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
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>

#include "common/type_utils.hpp"

#include "master/registrar.hpp"
#include "master/registry.hpp"

#include "state/protobuf.hpp"

using mesos::internal::state::protobuf::State;
using mesos::internal::state::protobuf::Variable;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait; // Necessary on some OS's to disambiguate.

using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Owned;
using process::Process;
using process::Promise;
using process::TLDR;
using process::USAGE;

using process::http::OK;

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
  RegistrarProcess(const Flags& _flags, State* _state)
    : ProcessBase(process::ID::generate("registrar")),
      updating(false),
      flags(_flags),
      state(_state) {}

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<Registry> recover(const MasterInfo& info);
  Future<bool> apply(Owned<Operation> operation);

protected:
  virtual void initialize()
  {
    route("/registry", registryHelp(), &RegistrarProcess::registry);
  }

private:
  // HTTP handlers.
  // /registrar(N)/registry
  Future<Response> registry(const Request& request);
  static string registryHelp();

  // The 'Recover' operation adds the latest MasterInfo.
  class Recover : public Operation
  {
  public:
    explicit Recover(const MasterInfo& _info) : info(_info) {}

  protected:
    virtual Try<bool> perform(Registry* registry, bool strict)
    {
      registry->mutable_master()->mutable_info()->CopyFrom(info);
      return true; // Mutation.
    }

  private:
    const MasterInfo info;
  };

  Option<Variable<Registry> > variable;
  deque<Owned<Operation> > operations;
  bool updating; // Used to signify fetching (recovering) or storing.

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

  const Flags flags;
  State* state;

  // Used to compose our operations with recovery.
  Option<Owned<Promise<Registry> > > recovered;
};


Future<Response> RegistrarProcess::registry(const Request& request)
{
  JSON::Object result;

  if (variable.isSome()) {
    result = JSON::Protobuf(variable.get().get());
  }

  return OK(result, request.query.get("jsonp"));
}


string RegistrarProcess::registryHelp()
{
  return HELP(
      TLDR(
          "Returns the current contents of the Registry in JSON."),
      USAGE(
          "/registrar(1)/registry"),
      DESCRIPTION(
          "Example:"
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
          "          { ",
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
          "          \"webui_hostname\": \"localhost\"",
          "        }",
          "      }",
          "    ]",
          "  }",
          "}",
          "```"));
}


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

  updating = true;

  LOG(INFO) << "Attempting to update the 'registry'";

  CHECK_SOME(variable);

  Registry registry = variable.get().get();

  foreach (Owned<Operation> operation, operations) {
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
    deque<Owned<Operation> > applied)
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
  while (!applied.empty()) {
    Owned<Operation> operation = applied.front();
    applied.pop_front();

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
  }

  applied.clear();

  if (!operations.empty()) {
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


Future<bool> Registrar::apply(Owned<Operation> operation)
{
  return dispatch(process, &RegistrarProcess::apply, operation);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
