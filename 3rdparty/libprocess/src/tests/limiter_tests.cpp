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

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/limiter.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>

using process::Clock;
using process::Future;
using process::RateLimiter;


TEST(LimiterTest, Acquire)
{
  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Clock::pause();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();
  Future<Nothing> acquire3 = limiter.acquire();

  AWAIT_READY(acquire1);

  Clock::advance(interval);
  AWAIT_READY(acquire2);
  EXPECT_TRUE(acquire3.isPending());

  Clock::advance(interval);
  AWAIT_READY(acquire3);
}


// In this test 4 permits are given, but the 2nd permit's acquire
// is immediately discarded. So, 1st, 3rd and 4th permits should
// be acquired according to the rate limit.
TEST(LimiterTest, DiscardMiddle)
{
  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Clock::pause();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();
  Future<Nothing> acquire3 = limiter.acquire();
  Future<Nothing> acquire4 = limiter.acquire();

  AWAIT_READY(acquire1);

  // Discard 'acquire2'.
  acquire2.discard();

  // 'acquire3' should be satisfied within one 'interval'.
  Clock::advance(interval);
  AWAIT_READY(acquire3);

  // 'acquire2' should be in 'DISCARDED' state.
  AWAIT_DISCARDED(acquire2);

  // 'acquire4' should be satisfied after another 'interval'.
  Clock::advance(interval);
  AWAIT_READY(acquire4);
}


// In this test 2 permits are initially given, but the 2nd permit's
// future is immediately discarded. Then the 3rd permit is given. So,
// 1st and 3rd permits should be acquired according to the rate limit.
TEST(LimiterTest, DiscardLast)
{
  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Clock::pause();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();

  AWAIT_READY(acquire1);

  // Discard 'acquire2'.
  acquire2.discard();

  // Now acquire 'acquire3'.
  Future<Nothing> acquire3 = limiter.acquire();

  // 'acquire3' should be satisfied within one 'interval'.
  Clock::advance(interval);
  AWAIT_READY(acquire3);
}
