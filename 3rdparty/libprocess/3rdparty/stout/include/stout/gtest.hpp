/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_GTEST_HPP__
#define __STOUT_GTEST_HPP__

#include <gtest/gtest.h>

#include <string>

#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>


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


template <typename T>
::testing::AssertionResult AssertSome(
    const char* expr,
    const Try<T>& actual)
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

#endif // __STOUT_GTEST_HPP__
