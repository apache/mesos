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

#include "test_module.hpp"

// Mesos core receives an object of type TestModuleImpl when
// instantiating the module.  The object is then used to make calls
// to foo() and bar() which are declared as part of the TestModule
// interface.
class TestModuleImpl : public TestModule
{
public:
  virtual int foo(char a, long b)
  {
    return a + b;
  }

  virtual int bar(float a, double b)
  {
    return a * b;
  }
};


static bool compatible()
{
  return true;
}


static TestModule* create()
{
  return new TestModuleImpl();
}


// Declares a module named 'example' of 'TestModule' kind.
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
