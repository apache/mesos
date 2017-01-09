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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <stout/os/su.hpp>

#include <stout/os/raw/argv.hpp>

#include <mesos/type_utils.hpp>

#include "common/parse.hpp"

#include "linux/capabilities.hpp"

#include "tests/containerizer/capabilities_test_helper.hpp"

using std::cerr;
using std::set;
using std::endl;
using std::string;
using std::vector;

using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;

namespace mesos {
namespace internal {
namespace tests {

const char CapabilitiesTestHelper::NAME[] = "Capabilities";


CapabilitiesTestHelper::Flags::Flags()
{
  add(&Flags::user,
      "user",
      "User to be used for the test.");

  add(&Flags::capabilities,
      "capabilities",
      "Capabilities to be set for the process.");
}


int CapabilitiesTestHelper::execute()
{
  Try<Capabilities> manager = Capabilities::create();
  if (manager.isError()) {
    cerr << "Failed to initialize capabilities manager: "
         << manager.error() << endl;

    return EXIT_FAILURE;
  }

  if (flags.capabilities.isNone()) {
    cerr << "Missing '--capabilities'" << endl;
    return EXIT_FAILURE;
  }

  if (flags.user.isSome()) {
    Try<Nothing> keepCaps = manager->setKeepCaps();
    if (keepCaps.isError()) {
      cerr << "Failed to set PR_SET_KEEPCAPS on the process: "
           << keepCaps.error() << endl;

      return EXIT_FAILURE;
    }

    Try<Nothing> su = os::su(flags.user.get());
    if (su.isError()) {
      cerr << "Failed to change user to '" << flags.user.get() << "'"
           << ": " << su.error() << endl;

      return EXIT_FAILURE;
    }

    // TODO(jieyu): Consider to clear PR_SET_KEEPCAPS.
  }

  Try<ProcessCapabilities> capabilities = manager->get();
  if (capabilities.isError()) {
    cerr << "Failed to get capabilities for the current process: "
         << capabilities.error() << endl;

    return EXIT_FAILURE;
  }

  // After 'os::su', 'effective' set is cleared. Since `SETPCAP` is
  // required in the `effective` set of a process to change the
  // bounding set, we need to restore it first.
  if (flags.user.isSome()) {
    capabilities->add(capabilities::EFFECTIVE, capabilities::SETPCAP);

    Try<Nothing> set = manager->set(capabilities.get());
    if (set.isError()) {
      cerr << "Failed to add SETPCAP to the effective set: "
           << set.error() << endl;

      return EXIT_FAILURE;
    }
  }

  set<Capability> target = capabilities::convert(flags.capabilities.get());

  capabilities->set(capabilities::EFFECTIVE,   target);
  capabilities->set(capabilities::PERMITTED,   target);
  capabilities->set(capabilities::INHERITABLE, target);
  capabilities->set(capabilities::BOUNDING,    target);

  Try<Nothing> set = manager->set(capabilities.get());
  if (set.isError()) {
    cerr << "Failed to set process capabilities: " << set.error() << endl;
    return EXIT_FAILURE;
  }

  vector<string> argv = {"ping", "-c", "1", "localhost"};

  // We use `ping` as a command since it has setuid bit set. This
  // allows us to test if capabilities whitelist works or not.
  ::execvp("ping", os::raw::Argv(argv));

  cerr << "'ping' failed: " << strerror(errno) << endl;

  return errno;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
