/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __TESTS_ASSERT_HPP__
#define __TESTS_ASSERT_HPP__

#include <gtest/gtest.h>

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>

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

#endif // __TESTS_ASSERT_HPP__
