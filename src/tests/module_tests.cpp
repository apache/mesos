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

#include <mesos/module.hpp>

#include <stout/dynamiclibrary.hpp>
#include <stout/os.hpp>

#include "examples/test_module.hpp"

#include "module/manager.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::string;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;
using namespace mesos::modules;

const char* DEFAULT_MODULE_LIBRARY_NAME = "examplemodule";
const char* DEFAULT_MODULE_NAME = "org_apache_mesos_TestModule";


class ModuleTest : public MesosTest
{
protected:
  // During the one-time setup of the test cases, we do the
  // following:
  // 1. set LD_LIBRARY_PATH to also point to the src/.libs directory.
  //    The original LD_LIBRARY_PATH is restored at the end of all
  //    tests.
  // 2. dlopen() examplemodule library and retrieve the pointer to
  //    ModuleBase for the test module. This pointer is later used to
  //    reset the Mesos and module API versions during per-test
  //    teardown.
  static void SetUpTestCase()
  {
    libraryDirectory = path::join(tests::flags.build_dir, "src", ".libs");

    // Get the current value of LD_LIBRARY_PATH.
    originalLdLibraryPath = os::libraries::paths();

    // Append our library path to LD_LIBRARY_PATH so that dlopen can
    // search the library directory for module libraries.
    os::libraries::appendPaths(libraryDirectory);

    EXPECT_SOME(dynamicLibrary.open(
        os::libraries::expandName(DEFAULT_MODULE_LIBRARY_NAME)));

    Try<void*> symbol = dynamicLibrary.loadSymbol(DEFAULT_MODULE_NAME);
    EXPECT_SOME(symbol);

    moduleBase = (ModuleBase*) symbol.get();
  }

  static void TearDownTestCase()
  {
    // Close the module library.
    dynamicLibrary.close();

    // Restore LD_LIBRARY_PATH environment variable.
    os::libraries::setPaths(originalLdLibraryPath);
  }

  ModuleTest()
    : module(None())
  {
    Modules::Library* library = defaultModules.add_libraries();
    library->set_file(path::join(
        libraryDirectory,
        os::libraries::expandName(DEFAULT_MODULE_LIBRARY_NAME)));
    library->add_modules(DEFAULT_MODULE_NAME);
  }

  // During the per-test tear-down, we unload the module to allow
  // later loads to succeed.
  ~ModuleTest()
  {
    // The TestModule instance is created by calling new. Let's
    // delete it to avoid memory leaks.
    if (module.isSome()) {
      delete module.get();
    }

    // Reset module API version and Mesos version in case the test
    // changed them.
    moduleBase->kind = "TestModule";
    moduleBase->moduleApiVersion = MESOS_MODULE_API_VERSION;
    moduleBase->mesosVersion = MESOS_VERSION;

    // Unload the module so a subsequent loading may succeed.
    ModuleManager::unload(DEFAULT_MODULE_NAME);
  }

  Modules defaultModules;
  Result<TestModule*> module;

  static DynamicLibrary dynamicLibrary;
  static ModuleBase* moduleBase;
  static string originalLdLibraryPath;
  static string libraryDirectory;
};


DynamicLibrary ModuleTest::dynamicLibrary;
ModuleBase* ModuleTest::moduleBase = NULL;
string ModuleTest::originalLdLibraryPath;
string ModuleTest::libraryDirectory;


static Modules getModules(const string& libraryName, const string& moduleName)
{
  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_file(os::libraries::expandName(libraryName));
  library->add_modules(moduleName);
  return modules;
}


// Test that a module library gets loaded,  and its contents
// version-verified. The provided test library matches the current
// Mesos version exactly.
TEST_F(ModuleTest, ExampleModuleLoadTest)
{
  EXPECT_SOME(ModuleManager::load(defaultModules));

  EXPECT_TRUE(ModuleManager::contains<TestModule>(DEFAULT_MODULE_NAME));
  module = ModuleManager::create<TestModule>(DEFAULT_MODULE_NAME);
  EXPECT_SOME(module);

  // The TestModuleImpl module's implementation of foo() returns
  // the sum of the passed arguments, whereas bar() returns the
  // product.
  EXPECT_EQ(module.get()->foo('A', 1024), 1089);
  EXPECT_EQ(module.get()->bar(0.5, 10.8), 5);
}


// Test that unloading a module succeeds if it has not been unloaded
// already.  Unloading unknown modules should fail as well.
TEST_F(ModuleTest, ExampleModuleUnloadTest)
{
  EXPECT_SOME(ModuleManager::load(defaultModules));

  module = ModuleManager::create<TestModule>(DEFAULT_MODULE_NAME);
  EXPECT_SOME(module);

  // Unloading the module should succeed the first time.
  EXPECT_SOME(ModuleManager::unload(DEFAULT_MODULE_NAME));

  // Unloading the same module a second time should fail.
  EXPECT_ERROR(ModuleManager::unload(DEFAULT_MODULE_NAME));

  // Unloading an unknown module should fail.
  EXPECT_ERROR(ModuleManager::unload("unknown"));
}


// Verify that loading a module of an invalid kind fails.
TEST_F(ModuleTest, InvalidModuleKind)
{
  moduleBase->kind = "NotTestModule";
  EXPECT_ERROR(ModuleManager::load(defaultModules));
}


// Test for correct author name, author email and library description.
TEST_F(ModuleTest, AuthorInfoTest)
{
  EXPECT_STREQ(moduleBase->authorName, "Apache Mesos");
  EXPECT_STREQ(moduleBase->authorEmail, "modules@mesos.apache.org");
  EXPECT_STREQ(moduleBase->description, "This is a test module.");
}


// Test that a module library gets loaded when provided with a
// library name without any extension and without the "lib" prefix.
TEST_F(ModuleTest, LibraryNameWithoutExtension)
{
  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_name(DEFAULT_MODULE_LIBRARY_NAME);
  library->add_modules(DEFAULT_MODULE_NAME);

  EXPECT_SOME(ModuleManager::load(modules));
}


// Test that a module library gets loaded with just library name if
// found in LD_LIBRARY_PATH.
TEST_F(ModuleTest, LibraryNameWithExtension)
{
  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_file(os::libraries::expandName(DEFAULT_MODULE_LIBRARY_NAME));
  library->add_modules(DEFAULT_MODULE_NAME);

  EXPECT_SOME(ModuleManager::load(modules));
}


// Test that module library loading fails when filename is empty.
TEST_F(ModuleTest, EmptyLibraryFilename)
{
  Modules modules = getModules("", "org_apache_mesos_TestModule");
  EXPECT_ERROR(ModuleManager::load(modules));
}


// Test that module library loading fails when module name is empty.
TEST_F(ModuleTest, EmptyModuleName)
{
  Modules modules = getModules("examplemodule", "");
  EXPECT_ERROR(ModuleManager::load(modules));
}


// Test that module library loading fails when given an unknown path.
TEST_F(ModuleTest, UnknownLibraryTest)
{
  Modules modules = getModules("unknown", "org_apache_mesos_TestModule");
  EXPECT_ERROR(ModuleManager::load(modules));
}


// Test that module loading fails when given an unknown module name on
// the commandline.
TEST_F(ModuleTest, UnknownModuleTest)
{
  Modules modules = getModules("examplemodule", "unknown");
  EXPECT_ERROR(ModuleManager::load(modules));
}


// Test that module instantiation fails when given an unknown module
// name.
TEST_F(ModuleTest, UnknownModuleInstantiationTest)
{
  EXPECT_SOME(ModuleManager::load(defaultModules));
  EXPECT_ERROR(ModuleManager::create<TestModule>("unknown"));
}


// Test that loading a non-module library fails.
TEST_F(ModuleTest, NonModuleLibrary)
{
  // Trying to load libmesos.so (libmesos.dylib on OS X) as a module
  // library should fail.
  Modules modules = getModules("mesos", DEFAULT_MODULE_NAME);
  EXPECT_ERROR(ModuleManager::load(modules));
}


// Test that loading a duplicate module fails.
TEST_F(ModuleTest, DuplicateModule)
{
  // Add duplicate module.
  Modules::Library* library = defaultModules.add_libraries();
  library->set_name(DEFAULT_MODULE_LIBRARY_NAME);
  library->add_modules(DEFAULT_MODULE_NAME);

  EXPECT_ERROR(ModuleManager::load(defaultModules));
}


// Test that loading a module library with a different API version
// fails
TEST_F(ModuleTest, DifferentApiVersion)
{
  // Make the API version '0'.
  moduleBase->moduleApiVersion = "0";
  EXPECT_ERROR(ModuleManager::load(defaultModules));

  // Make the API version arbitrarily high.
  moduleBase->moduleApiVersion = "1000";
  EXPECT_ERROR(ModuleManager::load(defaultModules));

  // Make the API version some random string.
  moduleBase->moduleApiVersion = "ThisIsNotAnAPIVersion!";
  EXPECT_ERROR(ModuleManager::load(defaultModules));
}


// Test that loading a module library compiled with a newer Mesos
// fails.
TEST_F(ModuleTest, NewerModuleLibrary)
{
  // Make the library version arbitrarily high.
  moduleBase->mesosVersion = "100.1.0";
  EXPECT_ERROR(ModuleManager::load(defaultModules));
}


// Test that loading a module library compiled with a really old
// Mesos fails.
TEST_F(ModuleTest, OlderModuleLibrary)
{
  // Make the library version arbitrarily low.
  moduleBase->mesosVersion = "0.1.0";
  EXPECT_ERROR(ModuleManager::load(defaultModules));
}
