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

#include "common/parse.hpp"
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

// TODO(karya): MESOS-4917: Replace the following non-pod static variables with
// pod equivalents. Cleanup further by introducing additional data structures to
// avoid keeping multiple mappings from module names.
hashmap<string, string> ModuleManager::kindToVersion;
hashmap<string, ModuleBase*> ModuleManager::moduleBases;
hashmap<string, Parameters> ModuleManager::moduleParameters;
hashmap<string, string> ModuleManager::moduleLibraries;
hashmap<string, DynamicLibrary*> ModuleManager::dynamicLibraries;


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
  kindToVersion["ContainerLogger"] = MESOS_VERSION;
  kindToVersion["Hook"] = MESOS_VERSION;
  kindToVersion["HttpAuthenticatee"] = MESOS_VERSION;
  kindToVersion["HttpAuthenticator"] = MESOS_VERSION;
  kindToVersion["Isolator"] = MESOS_VERSION;
  kindToVersion["MasterContender"] = MESOS_VERSION;
  kindToVersion["MasterDetector"] = MESOS_VERSION;
  kindToVersion["QoSController"] = MESOS_VERSION;
  kindToVersion["ResourceEstimator"] = MESOS_VERSION;
  kindToVersion["SecretResolver"] = MESOS_VERSION;
  kindToVersion["TestModule"] = MESOS_VERSION;
  kindToVersion["DiskProfileAdaptor"] = MESOS_VERSION;

  // What happens then when Mesos is built with a certain version,
  // 'kindToVersion' states a certain other minimum version, and a
  // module library is built against "module.hpp" belonging to yet
  // another Mesos version?
  //
  // Mesos can admit modules built against earlier versions of itself
  // by stating so explicitly in 'kindToVersion'.  If a module is
  // built with a Mesos version greater than or equal to the one
  // stated in 'kindToVersion', it passes this verification step.
  // Otherwise it is rejected when attempting to load it.
  //
  // Here are some examples:
  //
  // Mesos   kindToVersion    library    modules loadable?
  // 0.18.0      0.18.0       0.18.0          YES
  // 1.0.0       0.18.0       0.18.0          YES
  // 1.0.0       0.18.0       0.21.0          YES
  // 0.18.0      0.18.0       1.0.0           NO
  // 1.0.0       0.21.0       0.18.0          NO
  // 1.0.0       1.0.0        0.18.0          NO

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
  if (moduleBase->mesosVersion == nullptr ||
      moduleBase->moduleApiVersion == nullptr ||
      moduleBase->authorName == nullptr ||
      moduleBase->authorEmail == nullptr ||
      moduleBase->description == nullptr ||
      moduleBase->kind == nullptr) {
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

  if (moduleBase->compatible == nullptr) {
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


// This check is to ensure that the two modules, with the same module name, are
// indeed identical. We verify that they belong to the same module library and
// have identical attributes such as parameters, kind, version, etc.
//
// Notice that this check doesn't prevent one from having multiple instances of
// the same module. In the current implementation, it is up to the module
// developer to return an error if the module in question doesn't support
// multiple instances.
//
// TODO(karya): MESOS-4960: Enhance the module API to allow module developers to
// express whether the modules are multi-instantiable and thread-safe.
Try<Nothing> ModuleManager::verifyIdenticalModule(
    const string& libraryName,
    const Modules::Library::Module& module,
    const ModuleBase* base)
{
  const string& moduleName = module.name();

  // Verify that the two modules come from the same module library.
  CHECK(moduleLibraries.contains(moduleName));
  if (libraryName != moduleLibraries[moduleName]) {
    return Error(
        "The same module appears in two different module libraries - "
        "'" + libraryName + "' and '" + moduleLibraries[moduleName] + "'");
  }

  // Verify that the two modules contain the same set of parameters that appear
  // in the same order.
  CHECK(moduleParameters.contains(moduleName));
  const Parameters& parameters = moduleParameters[moduleName];
  bool parameterError =
    module.parameters().size() != parameters.parameter().size();

  for (int i = 0; i < module.parameters().size() && !parameterError; i++) {
    const Parameter& lhs = parameters.parameter().Get(i);
    const Parameter& rhs = module.parameters().Get(i);
    if (lhs.key() != rhs.key() || lhs.value() != rhs.value()) {
      parameterError = true;
    }
  }

  if (parameterError) {
    return Error(
        "A module with same name but different parameters already exists");
  }

  // Verify that the two `ModuleBase` definitions match.
  CHECK_NOTNULL(base);
  CHECK(moduleBases.contains(moduleName));
  ModuleBase* duplicateBase = moduleBases[moduleName];

  // TODO(karya): MESOS-4918: Cache module manifests to avoid potential
  // overwrite of `ModuleBase` fields by the module itself.
  if (strcmp(base->moduleApiVersion, duplicateBase->moduleApiVersion) != 0 ||
      strcmp(base->mesosVersion, duplicateBase->mesosVersion) != 0 ||
      strcmp(base->kind, duplicateBase->kind) != 0 ||
      strcmp(base->authorName, duplicateBase->authorName) != 0 ||
      strcmp(base->authorEmail, duplicateBase->authorEmail) != 0 ||
      strcmp(base->description, duplicateBase->description) != 0 ||
      base->compatible != duplicateBase->compatible) {
    return Error(
        "A module with same name but different module manifest already exists");
  }

  return Nothing();
}


Try<Nothing> ModuleManager::loadManifest(const Modules& modules)
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

        dynamicLibraries[libraryName] = dynamicLibrary.release();
      }

      // Load module manifests.
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (!module.has_name()) {
          return Error(
              "Error: module name not provided with library '" + libraryName +
              "'");
        }

        const string& moduleName = module.name();

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

        // We verify module compatibilty before checking for identical modules
        // to ensure that all the fields in the module manifest are valid before
        // we start comparing them with an already loaded manifest with the same
        // module name.
        if (moduleBases.contains(moduleName)) {
          Try<Nothing> result =
            verifyIdenticalModule(libraryName, module, moduleBase);

          if (result.isError()) {
            return Error(
                "Error loading module '" + moduleName + "'; this is"
                " potentially due to duplicate module names; " +
                result.error());
          }

          continue;
        }

        moduleBases[moduleName] = moduleBase;
        moduleLibraries[moduleName] = libraryName;

        // Now copy the supplied module-specific parameters.
        moduleParameters[moduleName].mutable_parameter()->CopyFrom(
            module.parameters());
      }
    }
  }

  return Nothing();
}


// We load the module manifests sequentially in an alphabetical order. If an
// error is encountered while processing a particular manifest, we do not load
// the remaining manifests and exit with the appropriate error message.
Try<Nothing> ModuleManager::load(const string& modulesDir)
{
  Try<list<string>> moduleManifests = os::ls(modulesDir);
  if (moduleManifests.isError()) {
    return Error(
        "Error loading module manifests from '" + modulesDir + "' directory: " +
        moduleManifests.error());
  }

  moduleManifests->sort();
  foreach (const string& filename, moduleManifests.get()) {
    const string filepath = path::join(modulesDir, filename);
    VLOG(1) << "Processing module manifest from '" << filepath << "'";

    Try<string> read = os::read(filepath);
    if (read.isError()) {
      return Error(
          "Error reading module manifest file '" + filepath + "': " +
          read.error());
    }

    Try<Modules> modules = flags::parse<Modules>(read.get());
    if (modules.isError()) {
      return Error(
          "Error parsing module manifest file '" + filepath + "': " +
          modules.error());
    }

    Try<Nothing> result = loadManifest(modules.get());
    if (result.isError()) {
      return Error(
          "Error loading modules from '" + filepath + "': " + result.error());
    }
  }

  return Nothing();
}
