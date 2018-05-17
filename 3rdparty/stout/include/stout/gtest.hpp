// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_GTEST_HPP__
#define __STOUT_GTEST_HPP__

#include <string>

#include <gtest/gtest.h>

#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/wait.h>
#endif

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif

template <typename T>
::testing::AssertionResult AssertSome(
    const char* expr,
    const Option<T>& actual)
{
  if (actual.isNone()) {
    return ::testing::AssertionFailure()
      << expr << " is NONE";
  }

  return ::testing::AssertionSuccess();
}


template <typename T, typename E>
::testing::AssertionResult AssertSome(
    const char* expr,
    const Try<T, E>& actual)
{
  if (actual.isError()) {
    return ::testing::AssertionFailure()
      << expr << ": " << actual.error();
  }

  return ::testing::AssertionSuccess();
}


template <typename T>
::testing::AssertionResult AssertSome(
    const char* expr,
    const Result<T>& actual)
{
  if (actual.isNone()) {
    return ::testing::AssertionFailure()
      << expr << " is NONE";
  } else if (actual.isError()) {
    return ::testing::AssertionFailure()
      << expr << ": " << actual.error();
  }

  return ::testing::AssertionSuccess();
}


template <typename T1, typename T2>
::testing::AssertionResult AssertSomeEq(
    const char* expectedExpr,
    const char* actualExpr,
    const T1& expected,
    const T2& actual) // Duck typing!
{
  const ::testing::AssertionResult result = AssertSome(actualExpr, actual);

  if (result) {
    if (expected == actual.get()) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: (" << actualExpr << ").get()\n"
        << "  Actual: " << ::testing::PrintToString(actual.get()) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


template <typename T1, typename T2>
::testing::AssertionResult AssertSomeNe(
    const char* notExpectedExpr,
    const char* actualExpr,
    const T1& notExpected,
    const T2& actual) // Duck typing!
{
  const ::testing::AssertionResult result = AssertSome(actualExpr, actual);

  if (result) {
    if (notExpected == actual.get()) {
      return ::testing::AssertionFailure()
        << "    Value of: (" << actualExpr << ").get()\n"
        << "      Actual: " << ::testing::PrintToString(actual.get()) << "\n"
        << "Not expected: " << notExpectedExpr << "\n"
        << "    Which is: " << ::testing::PrintToString(notExpected);
    } else {
      return ::testing::AssertionSuccess();
    }
  }

  return result;
}


#define ASSERT_SOME(actual)                     \
  ASSERT_PRED_FORMAT1(AssertSome, actual)


#define EXPECT_SOME(actual)                     \
  EXPECT_PRED_FORMAT1(AssertSome, actual)


#define ASSERT_SOME_EQ(expected, actual)                \
  ASSERT_PRED_FORMAT2(AssertSomeEq, expected, actual)


#define EXPECT_SOME_EQ(expected, actual)                \
  EXPECT_PRED_FORMAT2(AssertSomeEq, expected, actual)


#define ASSERT_SOME_NE(notExpected, actual)             \
  ASSERT_PRED_FORMAT2(AssertSomeNe, notExpected, actual)


#define EXPECT_SOME_NE(notExpected, actual)             \
  EXPECT_PRED_FORMAT2(AssertSomeNe, notExpected, actual)


#define ASSERT_SOME_TRUE(actual)                        \
  ASSERT_PRED_FORMAT2(AssertSomeEq, true, actual)


#define EXPECT_SOME_TRUE(actual)                        \
  EXPECT_PRED_FORMAT2(AssertSomeEq, true, actual)


#define ASSERT_SOME_FALSE(actual)                       \
  ASSERT_PRED_FORMAT2(AssertSomeEq, false, actual)


#define EXPECT_SOME_FALSE(actual)                       \
  EXPECT_PRED_FORMAT2(AssertSomeEq, false, actual)


#define ASSERT_ERROR(actual)                    \
  ASSERT_TRUE(actual.isError())


#define EXPECT_ERROR(actual)                    \
  EXPECT_TRUE(actual.isError())


#define ASSERT_NONE(actual)                     \
  ASSERT_TRUE(actual.isNone())


#define EXPECT_NONE(actual)                     \
  EXPECT_TRUE(actual.isNone())


// Creates a gtest `TEST` that is disabled on Windows.
// TODO(hausdorff): Remove after temporarily-disabled tests are fixed on
// Windows. See MESOS-6392.
#ifndef __WINDOWS__
#define TEST_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST(test_case_name, test_name)
#else
#define TEST_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST(test_case_name, DISABLED_##test_name)
#endif // __WINDOWS__


// Creates a gtest `TEST_F` that is disabled on Windows.
// TODO(hausdorff): Remove after temporarily-disabled tests are fixed on
// Windows. See MESOS-6392.
#ifndef __WINDOWS__
#define TEST_F_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST_F(test_case_name, test_name)
#else
#define TEST_F_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST_F(test_case_name, DISABLED_##test_name)
#endif // __WINDOWS__


// Creates a gtest `TEST_P` that is disabled on Windows.
// TODO(greggomann): Remove after temporarily-disabled tests are fixed on
// Windows. See MESOS-6392.
#ifndef __WINDOWS__
#define TEST_P_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST_P(test_case_name, test_name)
#else
#define TEST_P_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name) \
  TEST_P(test_case_name, DISABLED_##test_name)
#endif // __WINDOWS__


// Creates a gtest `TYPED_TEST` that is disabled on Windows.
// TODO(andschwa): Remove after temporarily-disabled tests are fixed on
// Windows. See MESOS-6392.
#ifndef __WINDOWS__
#define TYPED_TEST_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name)  \
  TYPED_TEST(test_case_name, test_name)
#else
#define TYPED_TEST_TEMP_DISABLED_ON_WINDOWS(test_case_name, test_name)  \
  TYPED_TEST(test_case_name, DISABLED_##test_name)
#endif // __WINDOWS__


// NOTE: On Windows, the closest equivalent to `sleep` is `timeout`.
// Unfortunately, `timeout` requires an interactive terminal, otherwise
// it errors out immediately. Instead, we use `ping` against localhost
// with a count.  On Windows, `ping` waits one second between pings.
// Additionally, because `ping` requires a count greater than 0,
// we simply `exit 0` if the sleep is too short.
//
// This must not be replaced with `powershell -c Start-Sleep`, because
// that causes flaky tests due to PowerShell crashing when a test
// machine is overloaded. See MESOS-8308.
#ifndef __WINDOWS__
#define SLEEP_COMMAND(x) "sleep " #x
#else
#define SLEEP_COMMAND(x) \
  ((x) > 0 ? "ping 127.0.0.1 -n " #x " > NUL" : "cmd /C exit 0")
#endif // __WINDOWS__


inline ::testing::AssertionResult AssertExited(
    const char* actualExpr,
    const int actual)
{
  if (WIFEXITED(actual)) {
    return ::testing::AssertionSuccess();
#ifndef __WINDOWS__
  } else if (WIFSIGNALED(actual)) {
    return ::testing::AssertionFailure()
      << "Expecting WIFEXITED(" << actualExpr << ") but "
      << " WIFSIGNALED(" << actualExpr << ") is true and "
      << "WTERMSIG(" << actualExpr << ") is " << strsignal(WTERMSIG(actual));
  } else if (WIFSTOPPED(actual)) {
    return ::testing::AssertionFailure()
      << "Expecting WIFEXITED(" << actualExpr << ") but"
      << " WIFSTOPPED(" << actualExpr << ") is true and "
      << "WSTOPSIG(" << actualExpr << ") is " << strsignal(WSTOPSIG(actual));
#endif // __WINDOWS__
  }

  return ::testing::AssertionFailure()
    << "Expecting WIFEXITED(" << actualExpr << ") but got"
    << " unknown value: " << ::testing::PrintToString(actual);
}


#define ASSERT_EXITED(expected, actual)                 \
  ASSERT_PRED_FORMAT2(AssertExited, expected, actual)


#define EXPECT_EXITED(expected, actual)                 \
  EXPECT_PRED_FORMAT2(AssertExited, expected, actual)


inline ::testing::AssertionResult AssertExitStatusEq(
    const char* expectedExpr,
    const char* actualExpr,
    const int expected,
    const int actual)
{
  const ::testing::AssertionResult result = AssertExited(actualExpr, actual);

  if (result) {
    if (WEXITSTATUS(actual) == expected) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: WEXITSTATUS(" << actualExpr << ")\n"
        << "  Actual: " << ::testing::PrintToString(WEXITSTATUS(actual)) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


#define ASSERT_WEXITSTATUS_EQ(expected, actual)                 \
  ASSERT_PRED_FORMAT2(AssertExitStatusEq, expected, actual)


#define EXPECT_WEXITSTATUS_EQ(expected, actual)                 \
  EXPECT_PRED_FORMAT2(AssertExitStatusEq, expected, actual)



inline ::testing::AssertionResult AssertExitStatusNe(
    const char* expectedExpr,
    const char* actualExpr,
    const int expected,
    const int actual)
{
  const ::testing::AssertionResult result = AssertExited(actualExpr, actual);

  if (result) {
    if (WEXITSTATUS(actual) != expected) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: WEXITSTATUS(" << actualExpr << ")\n"
        << "  Actual: " << ::testing::PrintToString(WEXITSTATUS(actual)) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


#define ASSERT_WEXITSTATUS_NE(expected, actual)                 \
  ASSERT_PRED_FORMAT2(AssertExitStatusNe, expected, actual)


#define EXPECT_WEXITSTATUS_NE(expected, actual)                 \
  EXPECT_PRED_FORMAT2(AssertExitStatusNe, expected, actual)


// Signals aren't used in Windows, so #ifdef these out.
#ifndef __WINDOWS__
inline ::testing::AssertionResult AssertSignaled(
    const char* actualExpr,
    const int actual)
{
  if (WIFEXITED(actual)) {
    return ::testing::AssertionFailure()
      << "Expecting WIFSIGNALED(" << actualExpr << ") but "
      << " WIFEXITED(" << actualExpr << ") is true and "
      << "WEXITSTATUS(" << actualExpr << ") is " << WEXITSTATUS(actual);
  } else if (WIFSIGNALED(actual)) {
    return ::testing::AssertionSuccess();
  } else if (WIFSTOPPED(actual)) {
    return ::testing::AssertionFailure()
      << "Expecting WIFSIGNALED(" << actualExpr << ") but"
      << " WIFSTOPPED(" << actualExpr << ") is true and "
      << "WSTOPSIG(" << actualExpr << ") is " << strsignal(WSTOPSIG(actual));
  }

  return ::testing::AssertionFailure()
    << "Expecting WIFSIGNALED(" << actualExpr << ") but got"
    << " unknown value: " << ::testing::PrintToString(actual);
}


#define ASSERT_SIGNALED(expected, actual)               \
  ASSERT_PRED_FORMAT2(AssertSignaled, expected, actual)


#define EXPECT_SIGNALED(expected, actual)               \
  EXPECT_PRED_FORMAT2(AssertSignaled, expected, actual)


inline ::testing::AssertionResult AssertTermSigEq(
    const char* expectedExpr,
    const char* actualExpr,
    const int expected,
    const int actual)
{
  const ::testing::AssertionResult result = AssertSignaled(actualExpr, actual);

  if (result) {
    if (WTERMSIG(actual) == expected) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: WTERMSIG(" << actualExpr << ")\n"
        << "  Actual: " << strsignal(WTERMSIG(actual)) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << strsignal(expected);
    }
  }

  return result;
}


#define ASSERT_WTERMSIG_EQ(expected, actual)                    \
  ASSERT_PRED_FORMAT2(AssertTermSigEq, expected, actual)


#define EXPECT_WTERMSIG_EQ(expected, actual)                    \
  EXPECT_PRED_FORMAT2(AssertTermSigEq, expected, actual)


inline ::testing::AssertionResult AssertTermSigNe(
    const char* expectedExpr,
    const char* actualExpr,
    const int expected,
    const int actual)
{
  const ::testing::AssertionResult result = AssertSignaled(actualExpr, actual);

  if (result) {
    if (WTERMSIG(actual) != expected) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: WTERMSIG(" << actualExpr << ")\n"
        << "  Actual: " << strsignal(WTERMSIG(actual)) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << strsignal(expected);
    }
  }

  return result;
}


#define ASSERT_WTERMSIG_NE(expected, actual)                    \
  ASSERT_PRED_FORMAT2(AssertTermSigNe, expected, actual)


#define EXPECT_WTERMSIG_NE(expected, actual)                    \
  EXPECT_PRED_FORMAT2(AssertTermSigNe, expected, actual)
#endif // __WINDOWS__

#endif // __STOUT_GTEST_HPP__
