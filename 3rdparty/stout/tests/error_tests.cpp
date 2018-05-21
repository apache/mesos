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

#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

using std::string;

using testing::StartsWith;

Error error1()
{
  return Error("Failed to ...");
}


Try<string> error2()
{
  return Error("Failed to ...");
}


Try<string> error3(const Try<string>& t)
{
  return t;
}


Result<string> error4()
{
  return Error("Failed to ...");
}


Result<string> error5(const Result<string>& r)
{
  return r;
}


TEST(ErrorTest, Test)
{
  Try<string> t = error1();
  EXPECT_ERROR(t);
  t = error2();
  EXPECT_ERROR(t);
  t = error3(error1());
  EXPECT_ERROR(t);

  Result<string> r = error1();
  EXPECT_ERROR(r);
  r = error4();
  EXPECT_ERROR(r);
  r = error5(error1());
  EXPECT_ERROR(r);
}


TEST(ErrorTest, Errno)
{
#ifdef __WINDOWS__
  DWORD einval = ERROR_INVALID_HANDLE;
#else
  int einval = EINVAL;
#endif

#ifdef __WINDOWS__
  DWORD notsock = WSAENOTSOCK;
#else
  int notsock = ENOTSOCK;
#endif

  EXPECT_EQ(einval, ErrnoError(einval).code);
  EXPECT_EQ(notsock, SocketError(notsock).code);

  EXPECT_EQ(einval, ErrnoError(einval, "errno error").code);
  EXPECT_THAT(
    ErrnoError(einval, "errno error").message, StartsWith("errno error"));

  EXPECT_EQ(notsock, SocketError(notsock, "socket error").code);
  EXPECT_THAT(
    SocketError(notsock, "socket error").message, StartsWith("socket error"));
}


#ifdef __WINDOWS__
TEST(ErrorTest, Windows)
{
  // NOTE: This is an edge case where the implementation explicitly
  // avoids calling `FormatMessage` when default constructed, and so
  // the message is an empty string, NOT "The operation completed
  // successfully."
  EXPECT_EQ(WindowsError(ERROR_SUCCESS).message, "");

  EXPECT_THAT(
    WindowsError(ERROR_FILE_NOT_FOUND).message,
    StartsWith("The system cannot find the file specified."));

  EXPECT_THAT(
    WindowsError(ERROR_INVALID_HANDLE).message,
    StartsWith("The handle is invalid."));

  EXPECT_THAT(
    WindowsError(ERROR_INVALID_PARAMETER).message,
    StartsWith("The parameter is incorrect."));
}
#endif // __WINDOWS__
