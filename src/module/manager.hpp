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

#ifndef __MODULE_MANAGER_HPP__
#define __MODULE_MANAGER_HPP__

#include <pthread.h>

#include <list>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/dynamiclibrary.hpp>
#include <stout/hashmap.hpp>

#include "common/lock.hpp"
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
  static Try<Nothing> load(const Modules& modules);

  // create() should be called only after load().
  template <typename T>
  static Try<T*> create(const std::string& moduleName)
  {
    mesos::Lock lock(&mutex);
    if (!moduleBases.contains(moduleName)) {
      return Error(
          "Module '" + moduleName + "' unknown");
    }

    Module<T>* module = (Module<T>*) moduleBases[moduleName];
    if (module->create == NULL) {
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

    T* instance = module->create(moduleParameters[moduleName]);
    if (instance == NULL) {
      return Error("Error creating Module instance for '" + moduleName + "'");
    }
    return instance;
  }

  template <typename T>
  static bool contains(const std::string& moduleName)
  {
    mesos::Lock lock(&mutex);
    return (moduleBases.contains(moduleName) &&
            moduleBases[moduleName]->kind == stringify(kind<T>()));
  }

  // Exposed just for testing so that we can unload a given
  // module  and remove it from the list of ModuleBases.
  static Try<Nothing> unload(const std::string& moduleName);

private:
  static void initialize();

  static Try<Nothing> verifyModule(
      const std::string& moduleName,
      const ModuleBase* moduleBase);

  // TODO(karya): Replace pthread_mutex_t with std::mutex in
  // common/lock.hpp and other places that refer to it.
  static pthread_mutex_t mutex;

  static hashmap<const std::string, std::string> kindToVersion;

  // Mapping from "module name" to the actual ModuleBase. If two
  // modules from different libraries have the same name then the last
  // one specified in the protobuf Modules will be picked.
  static hashmap<const std::string, ModuleBase*> moduleBases;

  // Module-specific command-line parameters.
  static hashmap<const std::string, Parameters> moduleParameters;

  // A list of dynamic libraries to keep the object from getting
  // destructed. Destroying the DynamicLibrary object could result in
  // unloading the library from the process memory.
  static hashmap<const std::string, process::Owned<DynamicLibrary>>
    dynamicLibraries;
};

} // namespace modules {
} // namespace mesos {

#endif // __MODULE_MANAGER_HPP__
