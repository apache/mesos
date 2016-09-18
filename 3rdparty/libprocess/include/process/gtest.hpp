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

#ifndef __PROCESS_GTEST_HPP__
#define __PROCESS_GTEST_HPP__

#include <gtest/gtest.h>

#include <string>

#include <process/check.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>

namespace process {

constexpr char READONLY_HTTP_AUTHENTICATION_REALM[] = "libprocess-readonly";
constexpr char READWRITE_HTTP_AUTHENTICATION_REALM[] = "libprocess-readwrite";

// A simple test event listener that makes sure to resume the clock
// after each test even if the previous test had a partial result
// (i.e., an ASSERT_* failed).
class ClockTestEventListener : public ::testing::EmptyTestEventListener
{
public:
  // Returns the singleton instance of the listener.
  static ClockTestEventListener* instance()
  {
    static ClockTestEventListener* listener = new ClockTestEventListener();
    return listener;
  }

  virtual void OnTestEnd(const ::testing::TestInfo&)
  {
    if (process::Clock::paused()) {
      process::Clock::resume();
    }
  }
private:
  ClockTestEventListener() {}
};


namespace internal {

// Returns true if the future becomes ready, discarded, or failed
// after the wait.
template <typename T>
bool await(const process::Future<T>& future, const Duration& duration)
{
  if (!process::Clock::paused()) {
    return future.await(duration);
  }

  // If the clock is paused, no new timers will expire.
  // Future::await(duration) may hang forever because it depends on
  // a timer to expire after 'duration'. We instead ensure all
  // expired timers are flushed and check if the future is satisfied.
  Stopwatch stopwatch;
  stopwatch.start();

  // Settle to make sure all expired timers are executed (not
  // necessarily finished, see below).
  process::Clock::settle();

  while (future.isPending() && stopwatch.elapsed() < duration) {
    // Synchronous operations and asynchronous process::Process
    // operations should finish when the above 'settle()' returns.
    // Other types of async operations such as io::write() may not.
    // Therefore we wait the specified duration for it to complete.
    // Note that nothing prevents the operations to schedule more
    // timeouts for some time in the future. These timeouts will
    // never be executed due to the paused process::Clock. In this
    // case we return after the stopwatch (system clock) runs out.
    os::sleep(Milliseconds(10));
  }

  return !future.isPending();
}

} // namespace internal {
} // namespace process {


template <typename T>
::testing::AssertionResult Await(
    const char* expr,
    const char*, // Unused string representation of 'duration'.
    const process::Future<T>& actual,
    const Duration& duration)
{
  if (!process::internal::await(actual, duration)) {
    return ::testing::AssertionFailure()
      << "Failed to wait " << duration << " for " << expr;
  }

  return ::testing::AssertionSuccess();
}


template <typename T>
::testing::AssertionResult AwaitAssertReady(
    const char* expr,
    const char*, // Unused string representation of 'duration'.
    const process::Future<T>& actual,
    const Duration& duration)
{
  if (!process::internal::await(actual, duration)) {
    return ::testing::AssertionFailure()
      << "Failed to wait " << duration << " for " << expr;
  } else if (actual.isDiscarded()) {
    return ::testing::AssertionFailure()
      << expr << " was discarded";
  } else if (actual.isFailed()) {
    return ::testing::AssertionFailure()
      << "(" << expr << ").failure(): " << actual.failure();
  }

  return ::testing::AssertionSuccess();
}


template <typename T>
::testing::AssertionResult AwaitAssertFailed(
    const char* expr,
    const char*, // Unused string representation of 'duration'.
    const process::Future<T>& actual,
    const Duration& duration)
{
  if (!process::internal::await(actual, duration)) {
    return ::testing::AssertionFailure()
      << "Failed to wait " << duration << " for " << expr;
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
::testing::AssertionResult AwaitAssertDiscarded(
    const char* expr,
    const char*, // Unused string representation of 'duration'.
    const process::Future<T>& actual,
    const Duration& duration)
{
  if (!process::internal::await(actual, duration)) {
    return ::testing::AssertionFailure()
      << "Failed to wait " << duration << " for " << expr;
  } else if (actual.isFailed()) {
    return ::testing::AssertionFailure()
      << "(" << expr << ").failure(): " << actual.failure();
  } else if (actual.isReady()) {
    return ::testing::AssertionFailure()
      << expr << " is ready (" << ::testing::PrintToString(actual.get()) << ")";
  }

  return ::testing::AssertionSuccess();
}


template <typename T1, typename T2>
::testing::AssertionResult AwaitAssertEq(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const T1& expected,
    const process::Future<T2>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

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


// TODO(bmahler): Differentiate EXPECT and ASSERT here.
#define AWAIT_FOR(actual, duration)             \
  ASSERT_PRED_FORMAT2(Await, actual, duration)


#define AWAIT(actual)                           \
  AWAIT_FOR(actual, Seconds(15))


#define AWAIT_ASSERT_READY_FOR(actual, duration)                \
  ASSERT_PRED_FORMAT2(AwaitAssertReady, actual, duration)


#define AWAIT_ASSERT_READY(actual)              \
  AWAIT_ASSERT_READY_FOR(actual, Seconds(15))


#define AWAIT_READY_FOR(actual, duration)       \
  AWAIT_ASSERT_READY_FOR(actual, duration)


#define AWAIT_READY(actual)                     \
  AWAIT_ASSERT_READY(actual)


#define AWAIT_EXPECT_READY_FOR(actual, duration)                \
  EXPECT_PRED_FORMAT2(AwaitAssertReady, actual, duration)


#define AWAIT_EXPECT_READY(actual)              \
  AWAIT_EXPECT_READY_FOR(actual, Seconds(15))


#define AWAIT_ASSERT_FAILED_FOR(actual, duration)               \
  ASSERT_PRED_FORMAT2(AwaitAssertFailed, actual, duration)


#define AWAIT_ASSERT_FAILED(actual)             \
  AWAIT_ASSERT_FAILED_FOR(actual, Seconds(15))


#define AWAIT_FAILED_FOR(actual, duration)       \
  AWAIT_ASSERT_FAILED_FOR(actual, duration)


#define AWAIT_FAILED(actual)                    \
  AWAIT_ASSERT_FAILED(actual)


#define AWAIT_EXPECT_FAILED_FOR(actual, duration)               \
  EXPECT_PRED_FORMAT2(AwaitAssertFailed, actual, duration)


#define AWAIT_EXPECT_FAILED(actual)             \
  AWAIT_EXPECT_FAILED_FOR(actual, Seconds(15))


#define AWAIT_ASSERT_DISCARDED_FOR(actual, duration)            \
  ASSERT_PRED_FORMAT2(AwaitAssertDiscarded, actual, duration)


#define AWAIT_ASSERT_DISCARDED(actual)                  \
  AWAIT_ASSERT_DISCARDED_FOR(actual, Seconds(15))


#define AWAIT_DISCARDED_FOR(actual, duration)       \
  AWAIT_ASSERT_DISCARDED_FOR(actual, duration)


#define AWAIT_DISCARDED(actual)                 \
  AWAIT_ASSERT_DISCARDED(actual)


#define AWAIT_EXPECT_DISCARDED_FOR(actual, duration)            \
  EXPECT_PRED_FORMAT2(AwaitAssertDiscarded, actual, duration)


#define AWAIT_EXPECT_DISCARDED(actual)                  \
  AWAIT_EXPECT_DISCARDED_FOR(actual, Seconds(15))


#define AWAIT_ASSERT_EQ_FOR(expected, actual, duration)                 \
  ASSERT_PRED_FORMAT3(AwaitAssertEq, expected, actual, duration)


#define AWAIT_ASSERT_EQ(expected, actual)       \
  AWAIT_ASSERT_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_EQ_FOR(expected, actual, duration)              \
  AWAIT_ASSERT_EQ_FOR(expected, actual, duration)


#define AWAIT_EQ(expected, actual)              \
  AWAIT_ASSERT_EQ(expected, actual)


#define AWAIT_EXPECT_EQ_FOR(expected, actual, duration)                 \
  EXPECT_PRED_FORMAT3(AwaitAssertEq, expected, actual, duration)


#define AWAIT_EXPECT_EQ(expected, actual)               \
  AWAIT_EXPECT_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_ASSERT_TRUE_FOR(actual, duration)                 \
  AWAIT_ASSERT_EQ_FOR(true, actual, duration)


#define AWAIT_ASSERT_TRUE(actual)       \
  AWAIT_ASSERT_EQ(true, actual)


#define AWAIT_TRUE_FOR(actual, duration)                 \
  AWAIT_ASSERT_TRUE_FOR(actual, duration)


#define AWAIT_TRUE(actual)       \
  AWAIT_ASSERT_TRUE(actual)


#define AWAIT_EXPECT_TRUE_FOR(actual, duration)               \
  AWAIT_EXPECT_EQ_FOR(true, actual, duration)


#define AWAIT_EXPECT_TRUE(actual)               \
  AWAIT_EXPECT_EQ(true, actual)


#define AWAIT_ASSERT_FALSE_FOR(actual, duration)                 \
  AWAIT_ASSERT_EQ_FOR(false, actual, duration)


#define AWAIT_ASSERT_FALSE(actual)       \
  AWAIT_ASSERT_EQ(false, actual)


#define AWAIT_FALSE_FOR(actual, duration)                 \
  AWAIT_ASSERT_FALSE_FOR(actual, duration)


#define AWAIT_FALSE(actual)       \
  AWAIT_ASSERT_FALSE(actual)


#define AWAIT_EXPECT_FALSE_FOR(actual, duration)               \
  AWAIT_EXPECT_EQ_FOR(false, actual, duration)


#define AWAIT_EXPECT_FALSE(actual)               \
  AWAIT_EXPECT_EQ(false, actual)


inline ::testing::AssertionResult AwaitAssertResponseStatusEq(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const std::string& expected,
    const process::Future<process::http::Response>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

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


#define AWAIT_ASSERT_RESPONSE_STATUS_EQ_FOR(expected, actual, duration) \
  ASSERT_PRED_FORMAT3(AwaitAssertResponseStatusEq, expected, actual, duration)


#define AWAIT_ASSERT_RESPONSE_STATUS_EQ(expected, actual)               \
  AWAIT_ASSERT_RESPONSE_STATUS_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_RESPONSE_STATUS_EQ_FOR(expected, actual, duration) \
  EXPECT_PRED_FORMAT3(AwaitAssertResponseStatusEq, expected, actual, duration)


#define AWAIT_EXPECT_RESPONSE_STATUS_EQ(expected, actual)               \
  AWAIT_EXPECT_RESPONSE_STATUS_EQ_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertResponseBodyEq(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const std::string& expected,
    const process::Future<process::http::Response>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

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


#define AWAIT_ASSERT_RESPONSE_BODY_EQ_FOR(expected, actual, duration)   \
  ASSERT_PRED_FORMAT3(AwaitAssertResponseBodyEq, expected, actual, duration)


#define AWAIT_ASSERT_RESPONSE_BODY_EQ(expected, actual)                 \
  AWAIT_ASSERT_RESPONSE_BODY_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_RESPONSE_BODY_EQ_FOR(expected, actual, duration)   \
  EXPECT_PRED_FORMAT3(AwaitAssertResponseBodyEq, expected, actual, duration)


#define AWAIT_EXPECT_RESPONSE_BODY_EQ(expected, actual)                 \
  AWAIT_EXPECT_RESPONSE_BODY_EQ_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertResponseHeaderEq(
    const char* expectedExpr,
    const char* keyExpr,
    const char* actualExpr,
    const char* durationExpr,
    const std::string& expected,
    const std::string& key,
    const process::Future<process::http::Response>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

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


#define AWAIT_ASSERT_RESPONSE_HEADER_EQ_FOR(expected, key, actual, duration) \
  ASSERT_PRED_FORMAT4(AwaitAssertResponseHeaderEq, expected, key, actual, duration) // NOLINT(whitespace/line_length)


#define AWAIT_ASSERT_RESPONSE_HEADER_EQ(expected, key, actual)          \
  AWAIT_ASSERT_RESPONSE_HEADER_EQ_FOR(expected, key, actual, Seconds(15))


#define AWAIT_EXPECT_RESPONSE_HEADER_EQ_FOR(expected, key, actual, duration) \
  EXPECT_PRED_FORMAT4(AwaitAssertResponseHeaderEq, expected, key, actual, duration) // NOLINT(whitespace/line_length)


#define AWAIT_EXPECT_RESPONSE_HEADER_EQ(expected, key, actual)          \
  AWAIT_EXPECT_RESPONSE_HEADER_EQ_FOR(expected, key, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertExited(
    const char* actualExpr,
    const char* durationExpr,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    if (actual->isNone()) {
      return ::testing::AssertionFailure()
        << "(" << actualExpr << ").get() is NONE";
    }

    return AssertExited(
        strings::join("(", actualExpr, ")->get()").c_str(),
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_EXITED_FOR(expected, actual, duration)             \
  ASSERT_PRED_FORMAT3(AwaitAssertExited, expected, actual, duration)


#define AWAIT_ASSERT_EXITED(expected, actual)                   \
  AWAIT_ASSERT_EXITED_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_EXITED_FOR(expected, actual, duration)             \
  EXPECT_PRED_FORMAT3(AwaitAssertExited, expected, actual, duration)


#define AWAIT_EXPECT_EXITED(expected, actual)                   \
  AWAIT_EXPECT_EXITED_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertExitStatusEq(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const int expected,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertExited(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    CHECK_SOME(actual.get());
    return AssertExitStatusEq(
        expectedExpr,
        strings::join("(", actualExpr, ")->get()").c_str(),
        expected,
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_WEXITSTATUS_EQ_FOR(expected, actual, duration)     \
  ASSERT_PRED_FORMAT3(AwaitAssertExitStatusEq, expected, actual, duration)


#define AWAIT_ASSERT_WEXITSTATUS_EQ(expected, actual)                   \
  AWAIT_ASSERT_WEXITSTATUS_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_WEXITSTATUS_EQ_FOR(expected, actual, duration)     \
  EXPECT_PRED_FORMAT3(AwaitAssertExitStatusEq, expected, actual, duration)


#define AWAIT_EXPECT_WEXITSTATUS_EQ(expected, actual)                   \
  AWAIT_EXPECT_WEXITSTATUS_EQ_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertExitStatusNe(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const int expected,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertExited(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    CHECK_SOME(actual.get());
    return AssertExitStatusNe(
        expectedExpr,
        strings::join("(", actualExpr, ")->get()").c_str(),
        expected,
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_WEXITSTATUS_NE_FOR(expected, actual, duration)     \
  ASSERT_PRED_FORMAT3(AwaitAssertExitStatusNe, expected, actual, duration)


#define AWAIT_ASSERT_WEXITSTATUS_NE(expected, actual)           \
  AWAIT_ASSERT_EXITSTATUS_NE_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_WEXITSTATUS_NE_FOR(expected, actual, duration)     \
  EXPECT_PRED_FORMAT3(AwaitAssertExitStatusNe, expected, actual, duration)


#define AWAIT_EXPECT_WEXITSTATUS_NE(expected, actual)                   \
  AWAIT_EXPECT_WEXITSTATUS_NE_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertSignaled(
    const char* actualExpr,
    const char* durationExpr,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertReady(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    if (actual->isNone()) {
      return ::testing::AssertionFailure()
        << "(" << actualExpr << ")->isNone() is true";
    }

    return AssertSignaled(
        strings::join("(", actualExpr, ")->get()").c_str(),
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_SIGNALED_FOR(expected, actual, duration)           \
  ASSERT_PRED_FORMAT3(AwaitAssertSignaled, expected, actual, duration)


#define AWAIT_ASSERT_SIGNALED(expected, actual)                 \
  AWAIT_ASSERT_SIGNALED_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_SIGNALED_FOR(expected, actual, duration)           \
  EXPECT_PRED_FORMAT3(AwaitAssertSignaled, expected, actual, duration)


#define AWAIT_EXPECT_SIGNALED(expected, actual)                 \
  AWAIT_EXPECT_SIGNALED_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertTermSigEq(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const int expected,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertSignaled(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    CHECK_SOME(actual.get());
    return AssertTermSigEq(
        expectedExpr,
        strings::join("(", actualExpr, ")->get()").c_str(),
        expected,
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_WTERMSIG_EQ_FOR(expected, actual, duration)        \
  ASSERT_PRED_FORMAT3(AwaitAssertTermSigEq, expected, actual, duration)


#define AWAIT_ASSERT_WTERMSIG_EQ(expected, actual)              \
  AWAIT_ASSERT_WTERMSIG_EQ_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_WTERMSIG_EQ_FOR(expected, actual, duration)        \
  EXPECT_PRED_FORMAT3(AwaitAssertTermSigEq, expected, actual, duration)


#define AWAIT_EXPECT_WTERMSIG_EQ(expected, actual)              \
  AWAIT_EXPECT_WTERMSIG_EQ_FOR(expected, actual, Seconds(15))


inline ::testing::AssertionResult AwaitAssertTermSigNe(
    const char* expectedExpr,
    const char* actualExpr,
    const char* durationExpr,
    const int expected,
    const process::Future<Option<int>>& actual,
    const Duration& duration)
{
  const ::testing::AssertionResult result =
    AwaitAssertSignaled(actualExpr, durationExpr, actual, duration);

  if (result) {
    CHECK_READY(actual);
    CHECK_SOME(actual.get());
    return AssertTermSigNe(
        expectedExpr,
        strings::join("(", actualExpr, ")->get()").c_str(),
        expected,
        actual->get());
  }

  return result;
}


#define AWAIT_ASSERT_WTERMSIG_NE_FOR(expected, actual, duration)        \
  ASSERT_PRED_FORMAT3(AwaitAssertTermSigNe, expected, actual, duration)


#define AWAIT_ASSERT_WTERMSIG_NE(expected, actual)              \
  AWAIT_ASSERT_TERMSIG_NE_FOR(expected, actual, Seconds(15))


#define AWAIT_EXPECT_TERMSIG_NE_FOR(expected, actual, duration)         \
  EXPECT_PRED_FORMAT3(AwaitAssertTermSigNe, expected, actual, duration)


#define AWAIT_EXPECT_WTERMSIG_NE(expected, actual)              \
  AWAIT_EXPECT_WTERMSIG_NE_FOR(expected, actual, Seconds(15))


// TODO(benh):
// inline ::testing::AssertionResult AwaitAssertStopped(...)
// inline ::testing::AssertionResult AwaitAssertStopSigEq(...)
// inline ::testing::AssertionResult AwaitAssertStopSigNe(...)

#endif // __PROCESS_GTEST_HPP__
