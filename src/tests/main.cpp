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

#include <gtest/gtest.h>

#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/socket.hpp> // For `wsa_*` on Windows.

#include "logging/logging.hpp"

#include "messages/messages.hpp" // For GOOGLE_PROTOBUF_VERIFY_VERSION.

#include "module/manager.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace mesos::internal;
using namespace mesos::internal::tests;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


#ifdef __WINDOWS__
// A no-op parameter validator. We use this to prevent the Windows
// implementation of the C runtime from calling `abort` during our test suite.
// See comment in `main.cpp`.
static void noop_invalid_parameter_handler(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file,
    unsigned int line,
    uintptr_t reserved)
{
  return;
}
#endif // __WINDOWS__


int main(int argc, char** argv)
{
#ifdef __WINDOWS__
  if (!net::wsa_initialize()) {
    cerr << "WSA failed to initialize" << endl;
    return EXIT_FAILURE;
  }

  // When we're running a debug build, the Windows implementation of the C
  // runtime will validate parameters passed to C-standard functions like
  // `::close`. When we are in debug mode, if a parameter is invalid, the
  // handler will usually call `abort`, rather than populating `errno` and
  // returning an error value. Since we expect some tests to pass invalid
  // paramaters to these functions, we disable this for testing.
  _set_invalid_parameter_handler(noop_invalid_parameter_handler);
#endif // __WINDOWS__

  GOOGLE_PROTOBUF_VERIFY_VERSION;

  using mesos::internal::tests::flags; // Needed to disambiguate.

  // Load flags from environment and command line but allow unknown
  // flags (since we might have gtest/gmock flags as well).
  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv, true);

  if (flags.help) {
    cout << flags.usage() << endl;
    testing::InitGoogleMock(&argc, argv); // Get usage from gtest too.
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  // Be quiet by default!
  if (!flags.verbose) {
    flags.quiet = true;
  }

  process::TEST_AWAIT_TIMEOUT = flags.test_await_timeout;

  // Initialize logging.
  logging::initialize(argv[0], true, flags);

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
  // Initialize Modules.
  if (flags.modules.isSome() && flags.modulesDir.isSome()) {
    EXIT(EXIT_FAILURE) <<
      flags.usage("Only one of --modules or --modules_dir should be specified");
  }

  if (flags.modulesDir.isSome()) {
    Try<Nothing> result =
      mesos::modules::ModuleManager::load(flags.modulesDir.get());
    if (result.isError()) {
      EXIT(EXIT_FAILURE) << "Error loading modules: " << result.error();
    }
  }

  Try<Nothing> result = tests::initModules(flags.modules);
  if (result.isError()) {
    EXIT(EXIT_FAILURE) << "Error initializing modules: " << result.error();
  }
#endif // __WINDOWS__

  // Disable /metrics/snapshot rate limiting, but do not
  // overwrite whatever the user set.
  os::setenv("LIBPROCESS_METRICS_SNAPSHOT_ENDPOINT_RATE_LIMIT", "", false);

  // If `process::initialize()` returns `false`, then it was called before this
  // invocation, meaning the authentication realm for libprocess-level HTTP
  // endpoints was set incorrectly. This should be the first invocation.
  if (!process::initialize(
          None(),
          READWRITE_HTTP_AUTHENTICATION_REALM,
          READONLY_HTTP_AUTHENTICATION_REALM)) {
    EXIT(EXIT_FAILURE) << "The call to `process::initialize()` in the tests' "
                       << "`main()` was not the function's first invocation";
  }

  // Initialize gmock/gtest.
  testing::InitGoogleMock(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";

  LOG(INFO) << "Source directory: " << flags.source_dir;
  LOG(INFO) << "Build directory: " << flags.build_dir;

  // Instantiate our environment. Note that it will be managed by
  // gtest after we add it via testing::AddGlobalTestEnvironment.
  environment = new tests::Environment(flags);

  testing::AddGlobalTestEnvironment(environment);

  const int test_results = RUN_ALL_TESTS();

  // Tear down the libprocess server sockets before we try to clean up the
  // Windows WSA stack. If we don't, this will cause worker threads to crash
  // the program on its way out.
  process::finalize();

  // Prefer to return the error code from the test run over the error code
  // from the WSA teardown. That is: if the test run failed, return that error
  // code; but, if the tests passed, we still want to return an error if the
  // WSA teardown failed. If both succeeded, return 0.
  const bool teardown_failed =
#ifdef __WINDOWS__
    !net::wsa_cleanup();
#else
    false;
#endif // __WINDOWS__

  return test_results > 0
    ? test_results
    : teardown_failed;
}
