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
