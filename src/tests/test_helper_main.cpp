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

#include <stout/none.hpp>
#include <stout/subcommand.hpp>

#include "tests/active_user_test_helper.hpp"
#include "tests/http_server_test_helper.hpp"
#include "tests/kill_policy_test_helper.hpp"

#ifndef __WINDOWS__
#include "tests/containerizer/memory_test_helper.hpp"
#endif // __WINDOWS__
#ifdef __linux__
#include "tests/containerizer/capabilities_test_helper.hpp"
#include "tests/containerizer/setns_test_helper.hpp"
#endif

using mesos::internal::tests::ActiveUserTestHelper;
using mesos::internal::tests::HttpServerTestHelper;
#ifndef __WINDOWS__
using mesos::internal::tests::KillPolicyTestHelper;
using mesos::internal::tests::MemoryTestHelper;
#endif // __WINDOWS__
#ifdef __linux__
using mesos::internal::tests::CapabilitiesTestHelper;
using mesos::internal::tests::SetnsTestHelper;
#endif


int main(int argc, char** argv)
{
  return Subcommand::dispatch(
      None(),
      argc,
      argv,
#ifdef __linux__
      new CapabilitiesTestHelper(),
      new SetnsTestHelper(),
#endif
      new HttpServerTestHelper(),
#ifndef __WINDOWS__
      new KillPolicyTestHelper(),
      new MemoryTestHelper(),
#endif // __WINDOWS__
      new ActiveUserTestHelper());
}
