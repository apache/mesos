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

#include <memory>
#include <vector>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#ifndef __WINDOWS__
#include <stout/os/signals.hpp>
#endif // __WINDOWS__

#include <stout/tests/environment.hpp>

using stout::internal::tests::Environment;
using stout::internal::tests::TestFilter;

using std::shared_ptr;
using std::vector;

// NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
// allocate any memory or grab locks. And according to
// https://code.google.com/p/google-glog/issues/detail?id=161
// it should work in 'most' cases in signal handlers.
inline void handler(int signal)
{
  RAW_LOG(FATAL, "Unexpected signal in signal handler: %d", signal);
}


int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

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

  vector<std::shared_ptr<TestFilter>> filters;
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
