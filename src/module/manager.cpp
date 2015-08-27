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

#include <mutex>
#include <string>
#include <vector>

#include <mesos/module.hpp>

#include <mesos/module/anonymous.hpp>
#include <mesos/module/module.hpp>

#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/stringify.hpp>
#include <stout/version.hpp>

#include "messages/messages.hpp"

#include "module/manager.hpp"

using std::list;
using std::string;
using std::vector;
using process::Owned;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::modules;

std::mutex ModuleManager::mutex;
hashmap<string, string> ModuleManager::kindToVersion;
hashmap<string, ModuleBase*> ModuleManager::moduleBases;
hashmap<string, Owned<DynamicLibrary>> ModuleManager::dynamicLibraries;
hashmap<string, Parameters> ModuleManager::moduleParameters;


void ModuleManager::initialize()
{
  // ATTENTION: Every time a Mesos developer breaks compatibility with
  // a module kind type, this table needs to be updated.
  // Specifically, the version value in the entry corresponding to the
  // kind needs to be set to the Mesos version that affects the
  // current change.  Typically that should be the version currently
  // under development.

  kindToVersion["Allocator"] = MESOS_VERSION;
  kindToVersion["Anonymous"] = MESOS_VERSION;
  kindToVersion["Authenticatee"] = MESOS_VERSION;
  kindToVersion["Authenticator"] = MESOS_VERSION;
  kindToVersion["Authorizer"] = MESOS_VERSION;
  kindToVersion["Hook"] = MESOS_VERSION;
  kindToVersion["Isolator"] = MESOS_VERSION;
  kindToVersion["QoSController"] = MESOS_VERSION;
  kindToVersion["ResourceEstimator"] = MESOS_VERSION;
  kindToVersion["TestModule"] = MESOS_VERSION;

  // What happens then when Mesos is built with a certain version,
  // 'kindToVersion' states a certain other minimum version, and a
  // module library is built against "module.hpp" belonging to yet
  // another Mesos version?
  //
  // Mesos can admit modules built against earlier versions of itself
  // by stating so explicitly in 'kindToVersion'.  If a modules is
  // built with a Mesos version greater than or equal to the one
  // stated in 'kindToVersion', it passes this verification step.
  // Otherwise it is rejected when attempting to load it.
  //
  // Here are some examples:
  //
  // Mesos   kindToVersion    library    modules loadable?
  // 0.18.0      0.18.0       0.18.0          YES
  // 0.29.0      0.18.0       0.18.0          YES
  // 0.29.0      0.18.0       0.21.0          YES
  // 0.18.0      0.18.0       0.29.0          NO
  // 0.29.0      0.21.0       0.18.0          NO
  // 0.29.0      0.29.0       0.18.0          NO

  // ATTENTION: This mechanism only protects the interfaces of
  // modules, not how they maintain functional compatibility with
  // Mesos and among each other.  This is covered by their own
  // "isCompatible" call.
}


// For testing only. Unload a given module and remove it from the list
// of ModuleBases.
Try<Nothing> ModuleManager::unload(const string& moduleName)
{
  synchronized (mutex) {
    if (!moduleBases.contains(moduleName)) {
      return Error(
          "Error unloading module '" + moduleName + "': module not loaded");
    }

    // Do not remove the dynamiclibrary as it could result in
    // unloading the library from the process memory.
    moduleBases.erase(moduleName);
  }
  return Nothing();
}


// TODO(karya): Show library author info for failed library/module.
Try<Nothing> ModuleManager::verifyModule(
    const string& moduleName,
    const ModuleBase* moduleBase)
{
  CHECK_NOTNULL(moduleBase);
  if (moduleBase->mesosVersion == NULL ||
      moduleBase->moduleApiVersion == NULL ||
      moduleBase->authorName == NULL ||
      moduleBase->authorEmail == NULL ||
      moduleBase->description == NULL ||
      moduleBase->kind == NULL) {
    return Error("Error loading module '" + moduleName + "'; missing fields");
  }

  // Verify module API version.
  if (stringify(moduleBase->moduleApiVersion) != MESOS_MODULE_API_VERSION) {
    return Error(
        "Module API version mismatch. Mesos has: " MESOS_MODULE_API_VERSION ", "
        "library requires: " + stringify(moduleBase->moduleApiVersion));
  }

  if (!kindToVersion.contains(moduleBase->kind)) {
    return Error("Unknown module kind: " + stringify(moduleBase->kind));
  }

  Try<Version> mesosVersion = Version::parse(MESOS_VERSION);
  CHECK_SOME(mesosVersion);

  Try<Version> minimumVersion = Version::parse(kindToVersion[moduleBase->kind]);
  CHECK_SOME(minimumVersion);

  Try<Version> moduleMesosVersion = Version::parse(moduleBase->mesosVersion);
  if (moduleMesosVersion.isError()) {
    return Error(moduleMesosVersion.error());
  }

  if (moduleMesosVersion.get() < minimumVersion.get()) {
    return Error(
        "Minimum supported mesos version for '" + stringify(moduleBase->kind) +
        "' is " + stringify(minimumVersion.get()) + ", but module is compiled "
        "with version " + stringify(moduleMesosVersion.get()));
  }

  if (moduleBase->compatible == NULL) {
    if (moduleMesosVersion.get() != mesosVersion.get()) {
      return Error(
          "Mesos has version " + stringify(mesosVersion.get()) +
          ", but module is compiled with version " +
          stringify(moduleMesosVersion.get()));
    }
    return Nothing();
  }

  if (moduleMesosVersion.get() > mesosVersion.get()) {
    return Error(
        "Mesos has version " + stringify(mesosVersion.get()) +
        ", but module is compiled with version " +
        stringify(moduleMesosVersion.get()));
  }

  bool result = moduleBase->compatible();
  if (!result) {
    return Error("Module " + moduleName + "has determined to be incompatible");
  }

  return Nothing();
}


Try<Nothing> ModuleManager::load(const Modules& modules)
{
  synchronized (mutex) {
    initialize();

    foreach (const Modules::Library& library, modules.libraries()) {
      string libraryName;
      if (library.has_file()) {
        libraryName = library.file();
      } else if (library.has_name()) {
        libraryName = os::libraries::expandName(library.name());
      } else {
        return Error("Library name or path not provided");
      }

      if (!dynamicLibraries.contains(libraryName)) {
        Owned<DynamicLibrary> dynamicLibrary(new DynamicLibrary());
        Try<Nothing> result = dynamicLibrary->open(libraryName);
        if (!result.isSome()) {
          return Error(
              "Error opening library: '" + libraryName +
              "': " + result.error());
        }

        dynamicLibraries[libraryName] = dynamicLibrary;
      }

      // Load module manifests.
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (!module.has_name()) {
          return Error(
              "Error: module name not provided with library '" + libraryName +
              "'");
        }

        // Check for possible duplicate module names.
        const std::string moduleName = module.name();
        if (moduleBases.contains(moduleName)) {
          return Error("Error loading duplicate module '" + moduleName + "'");
        }

        // Load ModuleBase.
        Try<void*> symbol =
          dynamicLibraries[libraryName]->loadSymbol(moduleName);
        if (symbol.isError()) {
          return Error(
              "Error loading module '" + moduleName + "': " + symbol.error());
        }
        ModuleBase* moduleBase = (ModuleBase*) symbol.get();

        // Verify module compatibility including version, etc.
        Try<Nothing> result = verifyModule(moduleName, moduleBase);
        if (result.isError()) {
          return Error(
              "Error verifying module '" + moduleName + "': " + result.error());
        }

        moduleBases[moduleName] = (ModuleBase*) symbol.get();

        // Now copy the supplied module-specific parameters.
        moduleParameters[moduleName].mutable_parameter()->CopyFrom(
            module.parameters());
      }
    }
  }

  return Nothing();
}
