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

#include "tests/kill_policy_test_helper.hpp"

#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>

#include <stout/duration.hpp>
#include <stout/os.hpp>

namespace mesos {
namespace internal {
namespace tests {

const char KillPolicyTestHelper::NAME[] = "KillPolicy";


void sigtermHandler(int signum)
{
  // Ignore SIGTERM.
  std::cerr << "Received SIGTERM" << std::endl;
}


KillPolicyTestHelper::Flags::Flags()
{
  add(&Flags::sleep_duration,
      "sleep_duration",
      "Number of seconds for which the helper will stay alive after having "
      "received a SIGTERM signal.");
}


// This test helper blocks until it receives a SIGTERM, then sleeps
// for a configurable amount of time before finally returning EXIT_SUCCESS.
int KillPolicyTestHelper::execute()
{
  // Setup the signal handler.
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = sigtermHandler;
  sigaction(SIGTERM, &action, nullptr);

  // Create a file in the sandbox which can be considered as
  // a signal that this test helper has been fully started.
  Try<Nothing> touch = os::touch(KillPolicyTestHelper::NAME);
  if (touch.isError()) {
    std::cerr << "Failed to create file '" << KillPolicyTestHelper::NAME
              << "': " << touch.error() << std::endl;
    return EXIT_FAILURE;
  }

  // Block the process until we get a signal.
  pause();

  os::sleep(Seconds(flags.sleep_duration));

  return EXIT_SUCCESS;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
