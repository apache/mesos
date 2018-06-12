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

#include <signal.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>

#include <stout/os/signals.hpp>

#include <stout/tests/environment.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::vector;

using stout::internal::tests::Environment;
using stout::internal::tests::TestFilter;

using std::shared_ptr;
using std::vector;

namespace {

class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::test_await_timeout,
        "test_await_timeout",
        "The default timeout for awaiting test events.",
        process::TEST_AWAIT_TIMEOUT);
  }

  Duration test_await_timeout;
};

} // namespace {


int main(int argc, char** argv)
{
  Flags flags;

  // Load flags from environment and command line but allow unknown
  // flags (since we might have gtest/gmock flags as well).
  Try<flags::Warnings> load = flags.load("LIBPROCESS_", argc, argv, true);

  if (flags.help) {
    cout << flags.usage() << endl;
    testing::InitGoogleMock(&argc, argv); // Get usage from gtest too.
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  process::TEST_AWAIT_TIMEOUT = flags.test_await_timeout;

  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

#if !GTEST_IS_THREADSAFE
  EXIT(EXIT_FAILURE) << "Testing environment is not thread safe, bailing!";
#endif // !GTEST_IS_THREADSAFE

  // Initialize libprocess.
  process::initialize(
      None(),
      process::READWRITE_HTTP_AUTHENTICATION_REALM,
      process::READONLY_HTTP_AUTHENTICATION_REALM);

  // NOTE: Windows does not support signal semantics required for these
  // handlers to be useful.
#ifndef __WINDOWS__
  // Install GLOG's signal handler.
  google::InstallFailureSignalHandler();

  // We reset the GLOG's signal handler for SIGTERM because
  // 'SubprocessTest.Status' sends SIGTERM to a subprocess which
  // results in a stack trace otherwise.
  os::signals::reset(SIGTERM);
#endif // __WINDOWS__

  vector<shared_ptr<TestFilter>> filters = {};
  Environment* environment = new Environment(filters);
  testing::AddGlobalTestEnvironment(environment);

  // Add the libprocess test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::ClockTestEventListener::instance());
  listeners.Append(process::FilterTestEventListener::instance());

  int result = RUN_ALL_TESTS();

  process::finalize(true);
  return result;
}
