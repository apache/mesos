#ifndef __PROCESS_GTEST_HPP__
#define __PROCESS_GTEST_HPP__

#include <gtest/gtest.h>

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/option.hpp>


template <typename T>
::testing::AssertionResult AssertFutureWillSucceed(
    const char* expr,
    const process::Future<T>& actual)
{
  if (!actual.await()) {
    return ::testing::AssertionFailure()
      << "Failed to wait for " << expr;
  } else if (actual.isDiscarded()) {
    return ::testing::AssertionFailure()
      << expr << " was discarded";
  } else if (actual.isFailed()) {
    return ::testing::AssertionFailure()
      << expr << ": " << actual.failure();
  }

  return ::testing::AssertionSuccess();
}


template <typename T>
::testing::AssertionResult AssertFutureWillFail(
    const char* expr,
    const process::Future<T>& actual)
{
  if (!actual.await()) {
    return ::testing::AssertionFailure()
      << "Failed to wait for " << expr;
  } else if (actual.isDiscarded()) {
    return ::testing::AssertionFailure()
      << expr << " was discarded";
  } else if (actual.isReady()) {
    return ::testing::AssertionFailure()
      << expr << " is ready (" << ::testing::PrintToString(actual.get()) << ")";
  }

  return ::testing::AssertionSuccess();
}


template <typename T>
::testing::AssertionResult AssertFutureWillDiscard(
    const char* expr,
    const process::Future<T>& actual)
{
  if (!actual.await()) {
    return ::testing::AssertionFailure()
      << "Failed to wait for " << expr;
  } else if (actual.isFailed()) {
    return ::testing::AssertionFailure()
      << expr << ": " << actual.failure();
  } else if (actual.isReady()) {
    return ::testing::AssertionFailure()
      << expr << " is ready (" << ::testing::PrintToString(actual.get()) << ")";
  }

  return ::testing::AssertionSuccess();
}


template <typename T1, typename T2>
::testing::AssertionResult AssertFutureWillEq(
    const char* expectedExpr,
    const char* actualExpr,
    const T1& expected,
    const process::Future<T2>& actual)
{
  const ::testing::AssertionResult result =
    AssertFutureWillSucceed(actualExpr, actual);

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


#define ASSERT_FUTURE_WILL_SUCCEED(actual)              \
  ASSERT_PRED_FORMAT1(AssertFutureWillSucceed, actual)


#define EXPECT_FUTURE_WILL_SUCCEED(actual)              \
  EXPECT_PRED_FORMAT1(AssertFutureWillSucceed, actual)


#define ASSERT_FUTURE_WILL_FAIL(actual)                 \
  ASSERT_PRED_FORMAT1(AssertFutureWillFail, actual)


#define EXPECT_FUTURE_WILL_FAIL(actual)                 \
  EXPECT_PRED_FORMAT1(AssertFutureWillFail, actual)


#define ASSERT_FUTURE_WILL_DISCARD(actual)              \
  ASSERT_PRED_FORMAT1(AssertFutureWillDiscard, actual)


#define EXPECT_FUTURE_WILL_DISCARD(actual)              \
  EXPECT_PRED_FORMAT1(AssertFutureWillDiscard, actual)


#define ASSERT_FUTURE_WILL_EQ(expected, actual)                 \
  ASSERT_PRED_FORMAT2(AssertFutureWillEq, expected, actual)


#define EXPECT_FUTURE_WILL_EQ(expected, actual)                 \
  EXPECT_PRED_FORMAT2(AssertFutureWillEq, expected, actual)


inline ::testing::AssertionResult AssertResponseStatusWillEq(
    const char* expectedExpr,
    const char* actualExpr,
    const std::string& expected,
    const process::Future<process::http::Response>& actual)
{
  const ::testing::AssertionResult result =
    AssertFutureWillSucceed(actualExpr, actual);

  if (result) {
    if (expected == actual.get().status) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: (" << actualExpr << ").get().status\n"
        << "  Actual: " << ::testing::PrintToString(actual.get().status) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


#define EXPECT_RESPONSE_STATUS_WILL_EQ(expected, actual)                \
  EXPECT_PRED_FORMAT2(AssertResponseStatusWillEq, expected, actual)


inline ::testing::AssertionResult AssertResponseBodyWillEq(
    const char* expectedExpr,
    const char* actualExpr,
    const std::string& expected,
    const process::Future<process::http::Response>& actual)
{
  const ::testing::AssertionResult result =
    AssertFutureWillSucceed(actualExpr, actual);

  if (result) {
    if (expected == actual.get().body) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: (" << actualExpr << ").get().body\n"
        << "  Actual: " << ::testing::PrintToString(actual.get().body) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


#define EXPECT_RESPONSE_BODY_WILL_EQ(expected, actual)           \
  EXPECT_PRED_FORMAT2(AssertResponseBodyWillEq, expected, actual)


inline ::testing::AssertionResult AssertResponseHeaderWillEq(
    const char* expectedExpr,
    const char* keyExpr,
    const char* actualExpr,
    const std::string& expected,
    const std::string& key,
    const process::Future<process::http::Response>& actual)
{
  const ::testing::AssertionResult result =
    AssertFutureWillSucceed(actualExpr, actual);

  if (result) {
    const Option<std::string> value = actual.get().headers.get(key);
    if (value.isNone()) {
      return ::testing::AssertionFailure()
        << "Response does not contain header '" << key << "'";
    } else if (expected == value.get()) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Value of: (" << actualExpr << ").get().headers[" << keyExpr << "]\n"
        << "  Actual: " << ::testing::PrintToString(value.get()) << "\n"
        << "Expected: " << expectedExpr << "\n"
        << "Which is: " << ::testing::PrintToString(expected);
    }
  }

  return result;
}


#define EXPECT_RESPONSE_HEADER_WILL_EQ(expected, key, actual)        \
  EXPECT_PRED_FORMAT3(AssertResponseHeaderWillEq, expected, key, actual)

#endif // __PROCESS_GTEST_HPP__
