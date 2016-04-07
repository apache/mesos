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

#include <string>

#include <stout/bytes.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "messages/messages.hpp"
#include "module/manager.hpp"

#include "tests/flags.hpp"
#include "tests/module.hpp"

using std::string;

using namespace mesos::modules;

namespace mesos {
namespace internal {
namespace tests {

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
static void addIsolatorModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test CPU and Memory isolator modules.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testisolator"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library, TestCpuIsolator, "org_apache_mesos_TestCpuIsolator");
  addModule(library, TestMemIsolator, "org_apache_mesos_TestMemIsolator");
}


// Add available Authentication modules.
static void addAuthenticationModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test authentication modules.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testauthentication"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library,
            TestCRAMMD5Authenticatee,
            "org_apache_mesos_TestCRAMMD5Authenticatee");
  addModule(library,
            TestCRAMMD5Authenticator,
            "org_apache_mesos_TestCRAMMD5Authenticator");
}


// Add available ContainerLogger modules.
static void addContainerLoggerModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Add our test container logger module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testcontainer_logger"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library,
            TestSandboxContainerLogger,
            "org_apache_mesos_TestSandboxContainerLogger");

  // Add the second container logger module.
  library = modules->add_libraries();
  library->set_file(getModulePath("logrotate_container_logger"));

  addModule(library,
            LogrotateContainerLogger,
            "org_apache_mesos_LogrotateContainerLogger");

  // Pass in the directory for the binary test sources.
  Modules::Library::Module* module = library->mutable_modules(0);
  mesos::Parameter* moduleParameter = module->add_parameters();
  moduleParameter->set_key("launcher_dir");
  moduleParameter->set_value(getLauncherDir());

  // Set the size and number of log files to keep.
  moduleParameter = module->add_parameters();
  moduleParameter->set_key("max_stdout_size");
  moduleParameter->set_value(stringify(Megabytes(2)));

  // NOTE: This is a 'logrotate' configuration option.
  // It means to "rotate" a file 4 times before removal.
  moduleParameter = module->add_parameters();
  moduleParameter->set_key("logrotate_stdout_options");
  moduleParameter->set_value("rotate 4");
}


static void addHookModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test hook module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testhook"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library, TestHook, "org_apache_mesos_TestHook");
}


static void addAnonymousModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test anonymous module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testanonymous"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library, TestAnonymous, "org_apache_mesos_TestAnonymous");
}


// Add available Allocator modules.
static void addAllocatorModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our allocator module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testallocator"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(library, TestDRFAllocator, "org_apache_mesos_TestDRFAllocator");
}


// Add available ResourceEstimator modules.
static void addResourceEstimatorModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our resource_estimator module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testresource_estimator"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library,
      TestNoopResourceEstimator,
      "org_apache_mesos_TestNoopResourceEstimator");
}


static void addAuthorizerModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test authorizer module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testauthorizer"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library, TestLocalAuthorizer, "org_apache_mesos_TestLocalAuthorizer");
}


static void addHttpAuthenticatorModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test HTTP authenticator module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testhttpauthenticator"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library,
      TestHttpBasicAuthenticator,
      "org_apache_mesos_TestHttpBasicAuthenticator");
}


static void addMasterContenderModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test anonymous module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testmastercontender"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library,
      TestMasterContender,
      "org_apache_mesos_TestMasterContender");
}


static void addMasterDetectorModules(Modules* modules)
{
  CHECK_NOTNULL(modules);

  // Now add our test anonymous module.
  Modules::Library* library = modules->add_libraries();
  library->set_file(getModulePath("testmasterdetector"));

  // To add a new module from this library, create a new ModuleID enum
  // and tie it with a module name.
  addModule(
      library,
      TestMasterDetector,
      "org_apache_mesos_TestMasterDetector");
}


Try<Nothing> initModules(const Option<Modules>& modules)
{
  // First get the user provided modules.
  Modules mergedModules;
  if (modules.isSome()) {
    mergedModules = modules.get();
  }

  // Add isolator modules from testisolator library.
  addIsolatorModules(&mergedModules);

  // Add authentication modules from testauthentication library.
  addAuthenticationModules(&mergedModules);

  // Add container logger modules from testcontainer_logger library.
  addContainerLoggerModules(&mergedModules);

  // Add hook modules from testhook library.
  addHookModules(&mergedModules);

  // Add anonymous modules from testanonymous library.
  addAnonymousModules(&mergedModules);

  // Add allocator modules from testallocator library.
  addAllocatorModules(&mergedModules);

  // Add resource estimator modules from testresource_estimator library.
  addResourceEstimatorModules(&mergedModules);

  // Add authorizer modules from testauthorizer library.
  addAuthorizerModules(&mergedModules);

  // Add HTTP authenticator modules from testhttpauthenticator library.
  addHttpAuthenticatorModules(&mergedModules);

  // Add MasterContender module from testmastercontender library.
  addMasterContenderModules(&mergedModules);

  // Add MasterDetector module from testmasterdetector library.
  addMasterDetectorModules(&mergedModules);

  return ModuleManager::load(mergedModules);
}


// Mapping from module ID to the actual module name.
Try<string> getModuleName(ModuleID id)
{
  if (!moduleNames.contains(id)) {
    return Error("Module '" + stringify(id) + "' not found");
  }
  return moduleNames[id];
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
