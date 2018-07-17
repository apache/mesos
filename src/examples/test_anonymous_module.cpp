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

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/anonymous.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "test_anonymous_module.hpp"

using namespace mesos;

using mesos::modules::Anonymous;

class TestAnonymous : public Anonymous
{
public:
  TestAnonymous()
  {
    VLOG(1) << "Anonymous module constructor";
    os::setenv(TEST_ANONYMOUS_ENVIRONMENT_VARIABLE, "42");
  }

  // TODO(tillt): Currently this destructor will only ever get called
  // during the test runs. Fix this behavior by introducing anonymous
  // module instance reference management.
  ~TestAnonymous() override
  {
    VLOG(1) << "Anonymous module destructor";
  }
};


static Anonymous* createAnonymous(const Parameters& parameters)
{
  return new TestAnonymous();
}


// Declares an Anonymous module named 'org_apache_mesos_TestAnonymous'.
mesos::modules::Module<Anonymous> org_apache_mesos_TestAnonymous(
  MESOS_MODULE_API_VERSION,
  MESOS_VERSION,
  "Apache Mesos",
  "modules@mesos.apache.org",
  "Test anonymous module.",
  nullptr,
  createAnonymous);
