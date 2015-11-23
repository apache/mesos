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

#include <errno.h> // For errno defines.

#include <limits>

#include <gtest/gtest.h>

#include <stout/os/strerror.hpp>


TEST(StrerrorTest, ValidErrno)
{
  EXPECT_EQ(::strerror(ENODEV), os::strerror(ENODEV));
  EXPECT_EQ(::strerror(ERANGE), os::strerror(ERANGE));
}


// Test that we behave correctly for invalid errnos
// where GLIBC does not use an internal buffer.
TEST(StrerrorTest, InvalidErrno)
{
  EXPECT_EQ(::strerror(-1), os::strerror(-1));

  // Check the longest possible "Unknown error N" error message.
  EXPECT_EQ(::strerror(std::numeric_limits<int>::max()),
            os::strerror(std::numeric_limits<int>::max()));
}
