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

#include "linux/cgroups2.hpp"

#include "slave/containerizer/mesos/isolators/cgroups2/cgroups2.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/core.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/cpu.hpp"

#include <set>
#include <string>

#include <process/id.hpp>

#include <stout/foreach.hpp>

using mesos::slave::Isolator;

using process::Owned;

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

Cgroups2IsolatorProcess::Cgroups2IsolatorProcess(
    const hashmap<string, Owned<Controller>>& _controllers)
    : ProcessBase(process::ID::generate("cgroups2-isolator")),
    controllers(_controllers) {}


Cgroups2IsolatorProcess::~Cgroups2IsolatorProcess() {}


Try<Isolator*> Cgroups2IsolatorProcess::create(const Flags& flags)
{
  hashmap<string, Try<Owned<ControllerProcess>>(*)(const Flags&)> creators = {
    {"core", &CoreControllerProcess::create},
    {"cpu", &CpuControllerProcess::create}
  };

  hashmap<string, Owned<Controller>> controllers;

  // The "core" controller is always enabled because the "cgroup.*" control
  // files which it interfaces with exist and are updated for all cgroups.
  set<string> controllersToCreate = { "core" };

  if (strings::contains(flags.isolation, "cgroups/all")) {
    Try<set<string>> available =
      ::cgroups2::controllers::available(::cgroups2::path(flags.cgroups_root));
    if (available.isError()) {
      return Error("Failed to determine the available cgroups v2 controllers: "
                   + available.error());
    }

    controllersToCreate = *available;
  } else {
    foreach (string isolator, strings::tokenize(flags.isolation, ",")) {
      if (!strings::startsWith(isolator, "cgroups/")) {
        // Skip when the isolator is not related to cgroups.
        continue;
      }

      isolator = strings::remove(isolator, "cgroups/", strings::Mode::PREFIX);
      if (!creators.contains(isolator)) {
        return Error(
            "Unknown or unsupported isolator 'cgroups/" + isolator + "'");
      }

      controllersToCreate.insert(isolator);
    }
  }

  foreach (const string& controllerName, controllersToCreate) {
    if (creators.count(controllerName) == 0) {
      return Error(
          "Cgroups v2 controller '" + controllerName + "' is not supported.");
    }

    Try<Owned<ControllerProcess>> process = creators.at(controllerName)(flags);
    if (process.isError()) {
      return Error("Failed to create controller '" + controllerName + "': "
                   + process.error());
    }

    Owned<Controller> controller = Owned<Controller>(new Controller(*process));
    controllers.put(controllerName, controller);
  }


  Owned<MesosIsolatorProcess> process(new Cgroups2IsolatorProcess(controllers));
  return new MesosIsolator(process);
}


bool Cgroups2IsolatorProcess::supportsNesting()
{
  // TODO(dleamy): Update this once cgroups v2 supports nested containers.
  return false;
}


bool Cgroups2IsolatorProcess::supportsStandalone()
{
  return true;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
