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

#include <mesos/module.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "test_module.hpp"

// Mesos core receives an object of type TestModuleImpl when
// instantiating the module.  The object is then used to make calls
// to foo() and bar() which are declared as part of the TestModule
// interface.
class TestModuleImpl : public TestModule
{
public:
  Try<Nothing> initialize(const mesos::Parameters& parameters) override
  {
    foreach (const mesos::Parameter& parameter, parameters.parameter()) {
      if (parameter.has_key() && parameter.has_value()) {
        flags[parameter.key()] = parameter.value();
      } else {
        return Error("Invalid key-value parameters");
      }
    }

    // We expect that when specifying the module, a module parameter
    // was also specified, i.e.:
    // "modules": [{"name": "org_apache_mesos_TestModule",
    //              "flags": [{"key": "operation", "value": "sum"}]}]
    // The expected value for the key "operation" is "sum".
    if (flags.contains("operation") && flags["operation"] != "sum") {
      return Error("Invalid 'operation'");
    }

    return Nothing();
  }

  int foo(char a, long b) override
  {
    return a + b;
  }

  int bar(float a, double b) override
  {
    return a * b;
  }

  int baz(int a, int b) override
  {
    if (flags["operation"] == "sum") {
      return a + b;
    }
    return -1;
  }

  mesos::Parameters parameters() const override
  {
    mesos::Parameters parameters;

    foreachpair (const std::string& key, const std::string& value, flags) {
      mesos::Parameter* parameter = parameters.add_parameter();
      parameter->set_key(key);
      parameter->set_value(value);
    }

    return parameters;
  }

private:
  hashmap<std::string, std::string> flags;
};


static bool compatible()
{
  return true;
}


static TestModule* create(const mesos::Parameters& parameters)
{
  TestModule *testModule = new TestModuleImpl();
  Try<Nothing> result = testModule->initialize(parameters);
  if (result.isError()) {
    delete testModule;
    // TODO(karya): make the return type Try<TestModule*> to pass the
    // error message as well.
    return nullptr;
  }
  return testModule;
}


// Declares a module named 'org_apache_mesos_TestModule' of
// 'TestModule' kind.
// compatible() hook is provided by the module for compatibility
// checks.
// create() hook returns an object of type 'TestModule'.
// Mesos core binds the module instance pointer as needed.
mesos::modules::Module<TestModule> org_apache_mesos_TestModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "This is a test module.",
    compatible,
    create);
