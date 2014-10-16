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


class ModuleTest : public MesosTest {};


static const string getLibraryDirectory()
{
  return path::join(tests::flags.build_dir, "src", ".libs");
}


static const string getLibraryPath(const string& libraryName)
{
  return path::join(
     getLibraryDirectory(),
     ModuleManager::expandLibraryName(libraryName));
}


static Modules getModules(const string& libraryName, const string& moduleName)
{
  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_file(getLibraryPath(libraryName));
  library->add_modules(moduleName);
  return modules;
}


// Test that a module library gets loaded,  and its contents
// version-verified. The provided test library matches the current
// Mesos version exactly. Parse the library and module request from a
// JSON string.
TEST_F(ModuleTest, ExampleModuleParseStringTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);
  EXPECT_SOME(ModuleManager::load(modules));

  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_SOME(module);

  // The TestModuleImpl module's implementation of foo() returns
  // the sum of the passed arguments, whereas bar() returns the
  // product.
  EXPECT_EQ(module.get()->foo('A', 1024), 1089);
  EXPECT_EQ(module.get()->bar(0.5, 10.8), 5);

  EXPECT_SOME(ModuleManager::unload(moduleName));
}


// Test for correct author name, author email and library description.
TEST_F(ModuleTest, AuthorInfoTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  DynamicLibrary library;
  EXPECT_SOME(library.open(getLibraryPath(libraryName)));

  // Check the return values against the values defined in
  // test_module_impl.cpp.
  Try<void*> symbol = library.loadSymbol(moduleName);
  EXPECT_SOME(symbol);

  ModuleBase* moduleBase = (ModuleBase*) symbol.get();
  EXPECT_EQ(stringify(moduleBase->authorName), "author");
  EXPECT_EQ(stringify(moduleBase->authorEmail), "author@email.com");
  EXPECT_EQ(stringify(moduleBase->description), "This is a test module.");
}


// Test that a module library gets loaded when provided with a
// library name without any extension and without the "lib" prefix.
TEST_F(ModuleTest, LibraryNameWithoutExtension)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";
  const string ldLibraryPath = "LD_LIBRARY_PATH";

  // Append our library path to LD_LIBRARY_PATH.
  const string oldLdLibraryPath = os::getenv(ldLibraryPath, false);
  const string newLdLibraryPath =
    getLibraryDirectory() + ":" + oldLdLibraryPath;
  os::setenv(ldLibraryPath, newLdLibraryPath);

  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_name(libraryName);
  library->add_modules(moduleName);

  EXPECT_SOME(ModuleManager::load(modules));

  // Reset LD_LIBRARY_PATH environment variable.
  os::setenv(ldLibraryPath, oldLdLibraryPath);

  EXPECT_SOME(ModuleManager::unload(moduleName));
}


// Test that a module library gets loaded with just library name if
// found in LD_LIBRARY_PATH.
TEST_F(ModuleTest, LibraryNameWithExtension)
{
  const string libraryName = ModuleManager::expandLibraryName("examplemodule");
  const string moduleName = "org_apache_mesos_TestModule";
  const string ldLibraryPath = "LD_LIBRARY_PATH";

  // Append our library path to LD_LIBRARY_PATH.
  const string oldLdLibraryPath = os::getenv(ldLibraryPath, false);
  const string newLdLibraryPath =
    getLibraryDirectory() + ":" + oldLdLibraryPath;
  os::setenv(ldLibraryPath, newLdLibraryPath);

  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_file(libraryName);
  library->add_modules(moduleName);

  EXPECT_SOME(ModuleManager::load(modules));

  // Reset LD_LIBRARY_PATH environment variable.
  os::setenv(ldLibraryPath, oldLdLibraryPath);

  EXPECT_SOME(ModuleManager::unload(moduleName));
}


// Test that module library loading fails when filename is empty.
TEST_F(ModuleTest, EmptyLibraryFilename)
{
  const string libraryName = "";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);

  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that module library loading fails when module name is empty.
TEST_F(ModuleTest, EmptyModuleName)
{
  const string libraryName = "examplemodule";
  const string moduleName = "";

  Modules modules = getModules(libraryName, moduleName);
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that module library loading fails when given an unknown path.
TEST_F(ModuleTest, UnknownLibraryTest)
{
  const string libraryName = "unknown";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);

  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that module loading fails when given an unknown module name on
// the commandline.
TEST_F(ModuleTest, UnknownModuleTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "unknown";

  Modules modules = getModules(libraryName, moduleName);
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that module instantiation fails when given an unknown module
// name.
TEST_F(ModuleTest, UnknownModuleInstantiationTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);
  EXPECT_SOME(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::create<TestModule>("unknown"));

  EXPECT_SOME(ModuleManager::unload(moduleName));
}


// Test that loading a non-module library fails.
TEST_F(ModuleTest, NonModuleLibrary)
{
  const string libraryName = "mesos";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that loading a duplicate module fails.
TEST_F(ModuleTest, DuplicateModule)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  Modules modules = getModules(libraryName, moduleName);

  // Add duplicate module.
  Modules::Library* library = modules.add_libraries();
  library->set_file(getLibraryPath(libraryName));
  library->add_modules(moduleName);

  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_SOME(ModuleManager::unload(moduleName));
}


// NOTE: We expect to pass 'version' which will outlive this function
// since we set it to the external module's 'mesosVersion'.
static void updateModuleLibraryVersion(
    DynamicLibrary* library,
    const string& moduleName,
    const char* version)
{
  Try<void*> symbol = library->loadSymbol(moduleName);
  EXPECT_SOME(symbol);

  ModuleBase* moduleBase = (ModuleBase*) symbol.get();
  moduleBase->mesosVersion = version;
}


// NOTE: Like above, we expect to pass 'version' which will outlive
// this function since we set it to the external module's
// 'mesosApiVersion'.
static void updateModuleApiVersion(
    DynamicLibrary* library,
    const string& moduleName,
    const char* version)
{
  Try<void*> symbol = library->loadSymbol(moduleName);
  EXPECT_SOME(symbol);

  ModuleBase* moduleBase = (ModuleBase*) symbol.get();
  moduleBase->moduleApiVersion = version;
}


// Test that loading a module library with a different API version
// fails
TEST_F(ModuleTest, DifferentApiVersion)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  DynamicLibrary library;
  EXPECT_SOME(library.open(getLibraryPath(libraryName)));

  Modules modules = getModules(libraryName, moduleName);

  // Make the API version '0'.
  updateModuleApiVersion(&library, moduleName, "0");
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));

  // Make the API version arbitrarily high.
  updateModuleApiVersion(&library, moduleName, "1000");
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));

  // Make the API version some random string.
  updateModuleApiVersion(&library, moduleName, "ThisIsNotAnAPIVersion!");
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that loading a module library compiled with a newer Mesos
// fails.
TEST_F(ModuleTest, NewerModuleLibrary)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  DynamicLibrary library;
  EXPECT_SOME(library.open(getLibraryPath(libraryName)));

  Modules modules = getModules(libraryName, moduleName);

  // Make the library version arbitrarily high.
  updateModuleLibraryVersion(&library, moduleName, "100.1.0");
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}


// Test that loading a module library compiled with a really old
// Mesos fails.
TEST_F(ModuleTest, OlderModuleLibrary)
{
  const string libraryName = "examplemodule";
  const string moduleName = "org_apache_mesos_TestModule";

  DynamicLibrary library;
  EXPECT_SOME(library.open(getLibraryPath(libraryName)));

  Modules modules = getModules(libraryName, moduleName);

  // Make the library version arbitrarily low.
  updateModuleLibraryVersion(&library, moduleName, "0.1.0");
  EXPECT_ERROR(ModuleManager::load(modules));

  EXPECT_ERROR(ModuleManager::unload(moduleName));
}
