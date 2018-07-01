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

#ifndef __MODULE_MANAGER_HPP__
#define __MODULE_MANAGER_HPP__

#include <list>
#include <mutex>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/module.hpp>

#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/dynamiclibrary.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace modules {

// Mesos module loading.
//
// Phases:

// 1. Load dynamic libraries that contain modules from the Modules
//    instance which may have come from a commandline flag.
// 2. Verify versions and compatibilities.
//   a) Library compatibility. (Module API version check)
//   b) Module compatibility. (Module Kind version check)
// 3. Instantiate singleton per module. (happens in the library)
// 4. Bind reference to use case. (happens in Mesos)


class ModuleManager
{
public:
  // Loads dynamic libraries, and verifies the compatibility of the
  // modules in them.
  //
  // NOTE: If loading fails at a particular library we don't unload
  // all of the already loaded libraries.
  static Try<Nothing> load(const Modules& modules)
  {
    return loadManifest(modules);
  }

  // NOTE: If loading fails at a particular library we don't unload
  // all of the already loaded libraries.
  static Try<Nothing> load(const std::string& modulesDir);

  // create() should be called only after load().
  template <typename T>
  static Try<T*> create(
      const std::string& moduleName,
      const Option<Parameters>& params = None())
  {
    synchronized (mutex) {
      if (!moduleBases.contains(moduleName)) {
        return Error(
            "Module '" + moduleName + "' unknown");
      }

      Module<T>* module = (Module<T>*) moduleBases[moduleName];
      if (module->create == nullptr) {
        return Error(
            "Error creating module instance for '" + moduleName + "': "
            "create() method not found");
      }

      std::string expectedKind = kind<T>();
      if (expectedKind != module->kind) {
        return Error(
            "Error creating module instance for '" + moduleName + "': "
            "module is of kind '" + module->kind + "', but the requested "
            "kind is '" + expectedKind + "'");
      }

      T* instance =
        module->create(
            params.isSome() ? params.get() : moduleParameters[moduleName]);
      if (instance == nullptr) {
        return Error("Error creating Module instance for '" + moduleName + "'");
      }
      return instance;
    }
  }

  template <typename T>
  static bool contains(const std::string& moduleName)
  {
    synchronized (mutex) {
      return (moduleBases.contains(moduleName) &&
              moduleBases[moduleName]->kind == stringify(kind<T>()));
    }
  }

  // Returns all module names that have been loaded that implement the
  // specified interface 'T'. For example:
  //
  //   std::vector<std::string> modules = ModuleManager::find<Hook>();
  //
  // Will return all of the module names for modules that implement
  // the `Hook` interface.
  template <typename T>
  static std::vector<std::string> find()
  {
    std::vector<std::string> names;

    synchronized (mutex) {
      foreachpair (const std::string& name, ModuleBase* base, moduleBases) {
        if (base->kind == stringify(kind<T>())) {
          names.push_back(name);
        }
      }
    }

    return names;
  }

  // Exposed just for testing so that we can unload a given
  // module and remove it from the list of `ModuleBase`.
  static Try<Nothing> unload(const std::string& moduleName);

private:
  static void initialize();

  static Try<Nothing> loadManifest(const Modules& modules);

  static Try<Nothing> verifyModule(
      const std::string& moduleName,
      const ModuleBase* moduleBase);

  // Allow multiple calls to `load()` by verifying that the modules with same
  // name are indeed identical. Thus, multiple loadings of a module manifest
  // are harmless.
  static Try<Nothing> verifyIdenticalModule(
      const std::string& libraryName,
      const Modules::Library::Module& module,
      const ModuleBase* base);

  static std::mutex mutex;

  static hashmap<std::string, std::string> kindToVersion;

  // Mapping from module name to the actual `ModuleBase`. If two
  // modules from different libraries have the same name then the last
  // one specified in the protobuf `Modules` will be picked.
  static hashmap<std::string, ModuleBase*> moduleBases;

  // Module-specific command-line parameters.
  static hashmap<std::string, Parameters> moduleParameters;

  // A list of dynamic libraries to keep the object from getting
  // destructed.
  // NOTE: We do leak loaded dynamic libraries. This is to allow
  // modules to make use of e.g., libprocess which otherwise could
  // lead to situations where libprocess (which we depend on ourself)
  // is unloaded before the destructor of below `static` is called.
  // Unloading the dynamic library could then lead to the linker
  // attempting to unload the libprocess loaded from the module which
  // is not there anymore.
  static hashmap<std::string, DynamicLibrary*> dynamicLibraries;

  // Module to library name mapping.
  static hashmap<std::string, std::string> moduleLibraries;
};

} // namespace modules {
} // namespace mesos {

#endif // __MODULE_MANAGER_HPP__
