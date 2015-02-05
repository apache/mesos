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

#include <string>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "messages/messages.hpp"
#include "module/manager.hpp"

#include "tests/flags.hpp"
#include "tests/module.hpp"

using std::string;

using namespace mesos;
using namespace mesos::tests;
using namespace mesos::modules;

static hashmap<ModuleID, string> moduleNames;


static void addModule(
    Modules::Library* library,
    ModuleID moduleId,
    string moduleName)
{
  moduleNames[moduleId] = moduleName;
  Modules::Library::Module* module = library->add_modules();
  module->set_name(moduleNames[moduleId]);
}


// Add available Isolator modules.
static void addIsolatorModules(Modules& modules)
{
  const string libraryPath = path::join(
      tests::flags.build_dir,
      "src",
      ".libs",
      os::libraries::expandName("testisolator"));

  // Now add our test CPU and Memory isolator modules.
  Modules::Library* library = modules.add_libraries();
  library->set_file(libraryPath);

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library, TestCpuIsolator, "org_apache_mesos_TestCpuIsolator");
  addModule(library, TestMemIsolator, "org_apache_mesos_TestMemIsolator");
}


// Add available Authentication modules.
static void addAuthenticationModules(Modules& modules)
{
  const string libraryPath = path::join(
      tests::flags.build_dir,
      "src",
      ".libs",
      os::libraries::expandName("testauthentication"));

  // Now add our test authentication modules.
  Modules::Library* library = modules.add_libraries();
  library->set_file(libraryPath);

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library,
            TestCRAMMD5Authenticatee,
            "org_apache_mesos_TestCRAMMD5Authenticatee");
  addModule(library,
            TestCRAMMD5Authenticator,
            "org_apache_mesos_TestCRAMMD5Authenticator");
}


static void addHookModules(Modules& modules)
{
  const string libraryPath = path::join(
      tests::flags.build_dir,
      "src",
      ".libs",
      os::libraries::expandName("testhook"));

  // Now add our test hook module.
  Modules::Library* library = modules.add_libraries();
  library->set_file(libraryPath);

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library, TestHook, "org_apache_mesos_TestHook");
}


Try<Nothing> tests::initModules(const Option<Modules>& modules)
{
  // First get the user provided modules.
  Modules mergedModules;
  if (modules.isSome()) {
    mergedModules = modules.get();
  }

  // Add isolator modules from testisolator library.
  addIsolatorModules(mergedModules);

  // Add authentication modules from testauthentication library.
  addAuthenticationModules(mergedModules);

  // Add hook modules from testhook library.
  addHookModules(mergedModules);

  return ModuleManager::load(mergedModules);
}


// Mapping from module ID to the actual module name.
Try<string> tests::getModuleName(ModuleID id)
{
  if (!moduleNames.contains(id)) {
    return Error("Module '" + stringify(id) + "' not found");
  }
  return moduleNames[id];
}
