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

#include <mesos/module.hpp>

#include <mesos/module/anonymous.hpp>

#include <process/owned.hpp>

#include <stout/os.hpp>
#include <stout/try.hpp>

#include "examples/test_anonymous_module.hpp"

#include "module/manager.hpp"

#include "tests/mesos.hpp"

using namespace mesos;

using namespace mesos::modules;

using std::string;
using std::vector;

using testing::_;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class AnonymousTest : public MesosTest {};


// Test for the side effect of our test-module which mutates the
// environment once it got loaded.
TEST_F_TEMP_DISABLED_ON_WINDOWS(AnonymousTest, Running)
{
  // Clear test relevant environment.
  os::unsetenv(TEST_ANONYMOUS_ENVIRONMENT_VARIABLE);

  // Creates an instance of all anonymous module implementations
  // loaded.
  vector<process::Owned<Anonymous>> modules;

  foreach (const string& name, ModuleManager::find<Anonymous>()) {
    Try<Anonymous*> create = ModuleManager::create<Anonymous>(name);
    ASSERT_SOME(create);
    modules.push_back(process::Owned<Anonymous>(create.get()));
  }

  // Test if the environment variables have been created by the
  // anonymous module.
  EXPECT_EQ("42", os::getenv(TEST_ANONYMOUS_ENVIRONMENT_VARIABLE).get());

  // Clear test relevant environment.
  os::unsetenv(TEST_ANONYMOUS_ENVIRONMENT_VARIABLE);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
