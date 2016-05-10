// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <glog/logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/exit.hpp>

#include <stout/os/socket.hpp> // For `wsa_*` on Windows.


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
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

#ifndef __WINDOWS__
  // Install glog's signal handler.
  // NOTE: this function is declared but not defined on Windows, so if we
  // attempt to compile this on Windows, we get a linker error.
  google::InstallFailureSignalHandler();
#else
  if (!net::wsa_initialize()) {
    EXIT(EXIT_FAILURE) << "WSA failed to initialize";
  }

  // When we're running a debug build, the Windows implementation of the C
  // runtime will validate parameters passed to C-standard functions like
  // `::close`. When we are in debug mode, if a parameter is invalid, the
  // handler will usually call `abort`, rather than populating `errno` and
  // returning an error value. Since we expect some tests to pass invalid
  // paramaters to these functions, we disable this for testing.
  _set_invalid_parameter_handler(noop_invalid_parameter_handler);
#endif // __WINDOWS__

  const int test_results = RUN_ALL_TESTS();

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
