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

#include <stout/dynamiclibrary.hpp>

#include <examples/test_module.hpp>

#include <mesos/module.hpp>

#include <module/manager.hpp>

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::string;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

class ModuleTest : public MesosTest {};

static const string getLibraryPath(string libraryName)
{
  return path::join(
      tests::flags.build_dir, "src", ".libs", "lib" + libraryName +
#ifdef __linux__
      ".so"
#else
      ".dylib"
#endif
      );
}


static Modules getModules(const string& libraryName, const string& moduleName)
{
  Modules modules;
  Modules::Library* library = modules.add_libraries();
  library->set_path(getLibraryPath(libraryName));
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
  const string moduleName = "exampleModule";
  Modules modules = getModules(libraryName, moduleName);
  ModuleManager::load(modules);

  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_SOME(module);

  // The TestModuleImpl module's implementation of foo() returns
  // the sum of the passed arguments, whereas bar() returns the
  // product.
  EXPECT_EQ(module.get()->foo('A', 1024), 1089);
  EXPECT_EQ(module.get()->bar(0.5, 10.8), 5);
  ModuleManager::unloadAll();
}


// Test for correct author name, author email and library description.
TEST_F(ModuleTest, AuthorInfoTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "exampleModule";
  DynamicLibrary library;
  Try<Nothing> result = library.open(getLibraryPath(libraryName));
  EXPECT_SOME(result);
  // Check the return values against the values defined in
  // test_module_impl.cpp.
  Try<void*> symbol = library.loadSymbol(moduleName);
  EXPECT_SOME(symbol);
  ModuleBase* moduleBase = (ModuleBase*) symbol.get();
  EXPECT_EQ(stringify(moduleBase->authorName), "author");
  EXPECT_EQ(stringify(moduleBase->authorEmail), "author@email.com");
  EXPECT_EQ(stringify(moduleBase->description), "This is a test module.");
  ModuleManager::unloadAll();
}


// Test that module library loading fails when given an unknown path.
TEST_F(ModuleTest, UnknownLibraryTest)
{
  const string libraryName = "unknown";
  const string moduleName = "exampleModule";
  Modules modules = getModules(libraryName, moduleName);
  ModuleManager::load(modules);

  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// Test that module loading fails when given an unknown module name on
// the commandline.
TEST_F(ModuleTest, UnknownModuleTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "unknown";
  Modules modules = getModules(libraryName, moduleName);
  ModuleManager::load(modules);

  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// Test that module instantiation fails when given an unknown module
// name.
TEST_F(ModuleTest, UnknownModuleInstantiationTest)
{
  const string libraryName = "examplemodule";
  const string moduleName = "exampleModule";
  Modules modules = getModules(libraryName, moduleName);
  ModuleManager::load(modules);

  Try<TestModule*> module = ModuleManager::create<TestModule>("unknown");
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// Test that loading a non-module library fails.
TEST_F(ModuleTest, NonModuleLibrary)
{
  const string libraryName = "mesos";
  const string moduleName = "exampleModule";
  Modules modules = getModules(libraryName, moduleName);
  ModuleManager::load(modules);

  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


static void updateModuleLibraryVersion(
    DynamicLibrary* library, const string& moduleName, Try<const char*> version)
{
  EXPECT_SOME(version);
  Try<void*> symbol = library->loadSymbol(moduleName);
  EXPECT_SOME(symbol);
  ModuleBase* moduleBase = (ModuleBase*) symbol.get();

  moduleBase->mesosVersion = version.get();
}


static void updateModuleApiVersion(
    DynamicLibrary* library, const string& moduleName, Try<const char*> version)
{
  EXPECT_SOME(version);
  Try<void*> symbol = library->loadSymbol(moduleName);
  EXPECT_SOME(symbol);
  ModuleBase* moduleBase = (ModuleBase*) symbol.get();
  moduleBase->moduleApiVersion = version.get();
}


// Test that loading a module library with a different API version
// fails
TEST_F(ModuleTest, DifferentApiVersion)
{
  const string libraryName = "examplemodule";
  const string moduleName = "exampleModule";
  DynamicLibrary library;
  Try<Nothing> result = library.open(getLibraryPath(libraryName));
  EXPECT_SOME(result);

  Modules modules = getModules(libraryName, moduleName);

  // Make the API version '0'.
  updateModuleApiVersion(&library, moduleName, "0");
  ModuleManager::load(modules);
  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();

  // Make the API version arbitrarily high.
  updateModuleApiVersion(&library, moduleName, "1000");
  ModuleManager::load(modules);
  module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();

  // Make the API version some random string.
  updateModuleApiVersion(&library, moduleName, "ThisIsNotAnAPIVersion!");
  ModuleManager::load(modules);
  module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// Test that loading a module library compiled with a newer Mesos
// fails.
TEST_F(ModuleTest, NewerModuleLibrary)
{
  const string libraryName = "examplemodule";
  const string moduleName = "exampleModule";
  DynamicLibrary library;
  Try<Nothing> result = library.open(getLibraryPath(libraryName));
  EXPECT_SOME(result);

  Modules modules = getModules(libraryName, moduleName);

  // Make the library version arbitrarily high.
  updateModuleLibraryVersion(&library, moduleName, "100.1.0");
  ModuleManager::load(modules);
  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// Test that loading a module library compiled with a really old
// Mesos fails.
TEST_F(ModuleTest, OlderModuleLibrary)
{
  const string libraryName = "examplemodule";
  const string moduleName = "exampleModule";
  DynamicLibrary library;
  Try<Nothing> result = library.open(getLibraryPath(libraryName));
  EXPECT_SOME(result);

  Modules modules = getModules(libraryName, moduleName);

  // Make the library version arbitrarily low.
  updateModuleLibraryVersion(&library, moduleName, "0.1.0");
  ModuleManager::load(modules);
  Try<TestModule*> module = ModuleManager::create<TestModule>(moduleName);
  EXPECT_ERROR(module);
  ModuleManager::unloadAll();
}


// TODO(bernd): Add MESOS_MODULE_IS_COMPATIBILE() tests.
