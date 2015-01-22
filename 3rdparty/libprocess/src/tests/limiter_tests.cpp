#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/limiter.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>
#include <stout/stopwatch.hpp>

using namespace process;

TEST(Limiter, Acquire)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Stopwatch stopwatch;
  stopwatch.start();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();
  Future<Nothing> acquire3 = limiter.acquire();

  AWAIT_READY(acquire1);

  AWAIT_READY(acquire2);
  ASSERT_LE(interval, stopwatch.elapsed());

  AWAIT_READY(acquire3);
  ASSERT_LE(interval * 2, stopwatch.elapsed());
}


// In this test 4 permits are given, but the 2nd permit's acquire
// is immediately discarded. So, 1st, 3rd and 4th permits should
// be acquired according to the rate limit.
TEST(Limiter, DiscardMiddle)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Stopwatch stopwatch;
  stopwatch.start();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();
  Future<Nothing> acquire3 = limiter.acquire();
  Future<Nothing> acquire4 = limiter.acquire();

  AWAIT_READY(acquire1);

  // Discard 'acquire2.
  acquire2.discard();

  // Wait until 'acquire3' is ready.
  AWAIT_READY(acquire3);

  // 'acquire2' should be in 'DISCARDED' state.
  AWAIT_DISCARDED(acquire2);

  // 'acquire3' should be satisfied within one 'interval'.
  ASSERT_LE(interval, stopwatch.elapsed());
  ASSERT_GE(interval * 2, stopwatch.elapsed());

  // 'acquire4' should be satisfied one 'interval' after
  // 'acquire3' is satisfied.
  AWAIT_READY(acquire4);
  ASSERT_LE(interval * 2, stopwatch.elapsed());
}


// In this test 2 permits are initially given, but the 2nd permit's
// future is immediately discarded. Then the 3rd permit is given. So,
// 1st and 3rd permits should be acquired according to the rate limit.
TEST(Limiter, DiscardLast)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Stopwatch stopwatch;
  stopwatch.start();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();

  AWAIT_READY(acquire1);

  // Discard 'acquire2'.
  acquire2.discard();

  // Now acquire 'acquire3'.
  Future<Nothing> acquire3 = limiter.acquire();

  // 'acquire3' should be satisfied one 'interval' after
  // 'acquire1' is satisfied.
  AWAIT_READY(acquire3);
  ASSERT_LE(interval, stopwatch.elapsed());
  ASSERT_GE(interval * 2, stopwatch.elapsed());
}
