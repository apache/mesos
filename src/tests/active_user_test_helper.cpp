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
// limitations under the License

#include <string>

#include <stout/os.hpp>

#include "tests/active_user_test_helper.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

const char ActiveUserTestHelper::NAME[] = "ActiveUser";


ActiveUserTestHelper::Flags::Flags()
{
  add(&Flags::user,
      "user",
      "The expected user name.");
}


// This test helper returns 0 if the current username equals the
// expected username. Returns 1 otherwise.
int ActiveUserTestHelper::execute()
{
  Result<string> user = os::user();
  if (user.isSome() && user.get() == flags.user) {
    return 0;
  }

  return 1;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
