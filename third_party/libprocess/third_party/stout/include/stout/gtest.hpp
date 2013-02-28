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


#define ASSERT_SOME(actual)                     \
  ASSERT_PRED_FORMAT1(AssertSome, actual)


#define EXPECT_SOME(actual)                     \
  EXPECT_PRED_FORMAT1(AssertSome, actual)


#define ASSERT_SOME_EQ(expected, actual)                \
  ASSERT_PRED_FORMAT2(AssertSomeEq, expected, actual)


#define EXPECT_SOME_EQ(expected, actual)                \
  EXPECT_PRED_FORMAT2(AssertSomeEq, expected, actual)


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

#endif // __STOUT_GTEST_HPP__
