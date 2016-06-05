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

#include <stout/os.hpp>

#include <stout/os/strerror.hpp>

using std::string;


#ifdef __WINDOWS__
const string errorMessage(int err)
{
  char buffer[1024];

  ::FormatMessage(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      err,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      buffer,
      sizeof(buffer),
      nullptr);

  return buffer;
}
#endif // __WINDOWS__


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


// The Linux documentation[1] on `herrno` indicates there are only 4 possible
// values that `h_errno` can have. Here we test that the Windows implementation
// will return "Unknown resolver error" for other error values.
//
// [1] http://linux.die.net/man/3/hstrerror
TEST(StrerrorTest, ValidHerrno)
{
#ifdef __WINDOWS__
  EXPECT_EQ("Unknown resolver error", os::hstrerror(ENODEV));
  EXPECT_EQ("Unknown resolver error", os::hstrerror(EINTR));
  EXPECT_EQ(::errorMessage(HOST_NOT_FOUND), os::hstrerror(HOST_NOT_FOUND));
  EXPECT_EQ(::errorMessage(NO_DATA), os::hstrerror(NO_DATA));
  EXPECT_EQ(::errorMessage(NO_RECOVERY), os::hstrerror(NO_RECOVERY));
  EXPECT_EQ(::errorMessage(TRY_AGAIN), os::hstrerror(TRY_AGAIN));
#else
  EXPECT_EQ(::hstrerror(ENODEV), os::hstrerror(ENODEV));
  EXPECT_EQ(::hstrerror(EINTR), os::hstrerror(EINTR));
  EXPECT_EQ(::hstrerror(HOST_NOT_FOUND), os::hstrerror(HOST_NOT_FOUND));
  EXPECT_EQ(::hstrerror(NO_DATA), os::hstrerror(NO_DATA));
  EXPECT_EQ(::hstrerror(NO_RECOVERY), os::hstrerror(NO_RECOVERY));
  EXPECT_EQ(::hstrerror(TRY_AGAIN), os::hstrerror(TRY_AGAIN));
#endif // __WINDOWS__
}
