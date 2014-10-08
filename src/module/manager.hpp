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

#include <messages/messages.hpp>

#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/dynamiclibrary.hpp>
#include <stout/hashmap.hpp>

#include "common/lock.hpp"

#include "mesos/mesos.hpp"

namespace mesos {
namespace internal {

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
  template <typename Kind>
  static Try<Kind*> create(const std::string& moduleName)
  {
    Lock lock(&mutex);
    if (!moduleBases.contains(moduleName)) {
      return Error(
          "Module '" + moduleName + "' unknown");
    }

    Module<Kind>* module = (Module<Kind>*) moduleBases[moduleName];
    if (module->create == NULL) {
      return Error(
          "Error creating Module instance for '" + moduleName + "': "
          "create() method not found");
    }
    Kind* kind = module->create();
    if (kind == NULL) {
      return Error("Error creating Module instance for '" + moduleName + "'");
    }
    return kind;
  }

  // Exposed just for testing so that we can close all open dynamic
  // libraries.
  static void unloadAll();

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

  // A list of dynamic libraries to keep the object from getting
  // destructed.
  // TODO(karya): Make it addressable only when we decide to implement
  // something that lets remove the module library.
  static std::list<process::Owned<DynamicLibrary> > dynamicLibraries;
};

} // namespace internal {
} // namespace mesos {

#endif // __MODULE_MANAGER_HPP__
