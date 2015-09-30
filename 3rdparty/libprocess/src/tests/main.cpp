/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <signal.h>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/os/signals.hpp>


// NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
// allocate any memory or grab locks. And according to
// https://code.google.com/p/google-glog/issues/detail?id=161
// it should work in 'most' cases in signal handlers.
inline void handler(int signal)
{
  if (signal == SIGPIPE) {
    RAW_LOG(WARNING, "Received signal SIGPIPE; escalating to SIGABRT");
    raise(SIGABRT);
  } else {
    RAW_LOG(FATAL, "Unexpected signal in signal handler: %d", signal);
  }
}


int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Initialize libprocess.
  process::initialize();

  // Install GLOG's signal handler.
  google::InstallFailureSignalHandler();

  // We reset the GLOG's signal handler for SIGTERM because
  // 'SubprocessTest.Status' sends SIGTERM to a subprocess which
  // results in a stack trace otherwise.
  os::signals::reset(SIGTERM);

  // Set up the SIGPIPE signal handler to escalate to SIGABRT
  // in order to have the glog handler catch it and print all
  // of its lovely information.
  os::signals::install(SIGPIPE, handler);

  // Add the libprocess test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::ClockTestEventListener::instance());
  listeners.Append(process::FilterTestEventListener::instance());

  int result = RUN_ALL_TESTS();

  process::finalize();
  return result;
}
