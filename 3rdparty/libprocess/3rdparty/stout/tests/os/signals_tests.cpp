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

#include <errno.h>

#include <string>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

using std::string;

// TODO(bmahler): Expose OsTest so this can use it.
class OsSignalsTest : public ::testing::Test {};


TEST_F(OsSignalsTest, Suppress)
{
  int pipes[2];

  ASSERT_NE(-1, pipe(pipes));

  ASSERT_SOME(os::close(pipes[0]));

  const string data = "hello";

  // Let's make sure we can suppress SIGPIPE. Note that this
  // only works on OS X because we are single threaded. In
  // multi-threaded applications, OS X delivers SIGPIPE to
  // the process, not necessarily to the triggering thread.
  SUPPRESS(SIGPIPE) {
    // Writing to a pipe that has been closed generates SIGPIPE.
    ASSERT_EQ(-1, write(pipes[1], data.c_str(), data.length()));

    ASSERT_EQ(EPIPE, errno);
  }

  ASSERT_SOME(os::close(pipes[1]));
}
