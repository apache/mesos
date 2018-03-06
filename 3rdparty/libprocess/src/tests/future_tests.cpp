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

#include <string>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;
using process::undiscardable;

using std::string;


TEST(FutureTest, Future)
{
  Promise<bool> promise;
  promise.set(true);
  ASSERT_TRUE(promise.future().isReady());
  EXPECT_TRUE(promise.future().get());
}


TEST(FutureTest, Stringify)
{
  Future<bool> future;
  EXPECT_EQ("Abandoned", stringify(future));

  {
    Owned<Promise<bool>> promise(new Promise<bool>());
    future = promise->future();
    promise.reset();
    EXPECT_EQ("Abandoned", stringify(future));
  }

  {
    Owned<Promise<bool>> promise(new Promise<bool>());
    future = promise->future();
    promise->future().discard();
    promise.reset();
    EXPECT_EQ("Abandoned (with discard)", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    EXPECT_EQ("Pending", stringify(future));
    promise.future().discard();
    EXPECT_EQ("Pending (with discard)", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.set(true);
    EXPECT_EQ("Ready", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.future().discard();
    promise.set(true);
    EXPECT_EQ("Ready (with discard)", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.fail("Failure");
    EXPECT_EQ("Failed: Failure", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.future().discard();
    promise.fail("Failure");
    EXPECT_EQ("Failed (with discard): Failure", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.discard();
    EXPECT_EQ("Discarded", stringify(future));
  }

  {
    Promise<bool> promise;
    future = promise.future();
    promise.future().discard();
    promise.discard();
    EXPECT_EQ("Discarded (with discard)", stringify(future));
  }
}


TEST(FutureTest, Associate)
{
  Promise<bool> promise1;
  Future<bool> future1(true);
  promise1.associate(future1);
  ASSERT_TRUE(promise1.future().isReady());
  EXPECT_TRUE(promise1.future().get());

  Promise<bool> promise2;
  Promise<bool> promise2_;
  Future<bool> future2 = promise2_.future();
  promise2.associate(future2);
  promise2_.discard();
  ASSERT_TRUE(promise2.future().isDiscarded());

  Promise<bool> promise3;
  Promise<bool> promise4;
  promise3.associate(promise4.future());
  promise4.fail("associate");
  ASSERT_TRUE(promise3.future().isFailed());
  EXPECT_EQ("associate", promise3.future().failure());

  // Test 'discard' versus 'discarded' after association.
  Promise<bool> promise5;
  Future<bool> future3;
  promise5.associate(future3);
  EXPECT_FALSE(future3.isDiscarded());
  promise5.future().discard();
  EXPECT_TRUE(future3.hasDiscard());

  Promise<bool> promise6;
  Promise<bool> promise7;
  promise6.associate(promise7.future());
  promise7.discard();
  EXPECT_TRUE(promise6.future().isDiscarded());
}


void onAny(const Future<bool>& future, bool* b)
{
  ASSERT_TRUE(future.isReady());
  *b = future.get();
}


TEST(FutureTest, OnAny)
{
  bool b = false;
  Future<bool>(true)
    .onAny(lambda::bind(&onAny, lambda::_1, &b));
  EXPECT_TRUE(b);
}


static Future<string> itoa1(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


static string itoa2(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


TEST(FutureTest, Then)
{
  Promise<int*> promise;

  int i = 42;

  promise.set(&i);

  Future<string> future = promise.future()
    .then(lambda::bind(&itoa1, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());

  future = promise.future()
    .then(lambda::bind(&itoa2, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());
}


TEST(FutureTest, CallableOnce)
{
  Promise<Nothing> promise;
  promise.set(Nothing());

  Future<int> future = promise.future()
    .then(lambda::partial(
        [](std::unique_ptr<int>&& o) {
          return *o;
        },
        std::unique_ptr<int>(new int(42))));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(42, future.get());

  int n = 0;
  future = promise.future()
    .onReady(lambda::partial(
        [&n](std::unique_ptr<int> o) {
          n += *o;
        },
        std::unique_ptr<int>(new int(1))))
    .onAny(lambda::partial(
        [&n](std::unique_ptr<int>&& o) {
          n += *o;
        },
        std::unique_ptr<int>(new int(10))))
    .onFailed(lambda::partial(
        [&n](const std::unique_ptr<int>& o) {
          n += *o;
        },
        std::unique_ptr<int>(new int(100))))
    .onDiscard(lambda::partial(
        [&n](std::unique_ptr<int>&& o) {
          n += *o;
        },
        std::unique_ptr<int>(new int(1000))))
    .onDiscarded(lambda::partial(
        [&n](std::unique_ptr<int>&& o) {
          n += *o;
        },
        std::unique_ptr<int>(new int(10000))))
    .then([&n]() { return n; });

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(11, future.get());
}


Future<int> repair(const Future<int>& future)
{
  EXPECT_TRUE(future.isFailed());
  EXPECT_EQ("Failure", future.failure());
  return 43;
}


// Checks that 'repair' callback gets executed if the future failed
// and not executed if the future is completed successfully.
TEST(FutureTest, Repair)
{
  // Check that the 'repair' callback _does not_ get executed by
  // making sure that when we complete the promise with a value that's
  // the value that we get back.
  Promise<int> promise1;

  Future<int> future1 = promise1.future()
    .repair(lambda::bind(&repair, lambda::_1));

  EXPECT_TRUE(future1.isPending());

  promise1.set(42); // So this means 'repair' should not get executed.

  AWAIT_EXPECT_EQ(42, future1);

  // Check that the 'repair' callback gets executed by failing the
  // promise which should invoke the 'repair' callback.
  Promise<int> promise2;

  Future<int> future2 = promise2.future()
    .repair(lambda::bind(&repair, lambda::_1));

  EXPECT_TRUE(future2.isPending());

  promise2.fail("Failure"); // So 'repair' should get called returning '43'.

  AWAIT_EXPECT_EQ(43, future2);
}



Future<Nothing> after(std::atomic_bool* executed, const Future<Nothing>& future)
{
  EXPECT_TRUE(future.hasDiscard());
  executed->store(true);
  return Failure("Failure");
}


// Checks that the 'after' callback gets executed if the future is not
// completed.
TEST(FutureTest, After1)
{
  Clock::pause();

  std::atomic_bool executed(false);

  Future<Nothing> future = Future<Nothing>()
    .after(Hours(42), lambda::bind(&after, &executed, lambda::_1));

  // A pending future should stay pending until 'after' is executed.
  EXPECT_TRUE(future.isPending());

  // Only advanced halfway, future should remain pending.
  Clock::advance(Hours(21));

  EXPECT_TRUE(future.isPending());

  // Even doing a discard on the future should keep it pending.
  future.discard();

  EXPECT_TRUE(future.isPending());

  // After advancing all the way the future should now fail because
  // the 'after' callback gets executed.
  Clock::advance(Hours(21));

  AWAIT_FAILED(future);

  EXPECT_TRUE(executed.load());

  Clock::resume();
}


// Checks that completing a promise will keep the 'after' callback
// from executing.
TEST(FutureTest, After2)
{
  Clock::pause();

  std::atomic_bool executed(false);

  Promise<Nothing> promise;

  Future<Nothing> future = promise.future()
    .after(Hours(42), lambda::bind(&after, &executed, lambda::_1));

  EXPECT_TRUE(future.isPending());

  // Only advanced halfway, future should remain pending.
  Clock::advance(Hours(21));

  EXPECT_TRUE(future.isPending());

  // Even doing a discard on the future should keep it pending.
  future.discard();

  EXPECT_TRUE(future.isPending());

  // Now set the promise, the 'after' timer should be cancelled and
  // the pending future should be completed.
  promise.set(Nothing());

  AWAIT_READY(future);

  // Advancing time the rest of the way should not cause the 'after'
  // callback to execute.
  Clock::advance(Hours(21));

  EXPECT_FALSE(executed.load());

  Clock::resume();
}


// Verifies that a a future does not leak memory after calling
// `after()`. This behavior occurred because a future indirectly
// kept a reference counted pointer to itself.
TEST(FutureTest, After3)
{
  Future<Nothing> future;
  process::WeakFuture<Nothing> weak_future(future);

  EXPECT_SOME(weak_future.get());

  {
    Clock::pause();

    // The original future disappears here. After this call the
    // original future goes out of scope and should not be reachable
    // anymore.
    future = future
      .after(Milliseconds(1), [](Future<Nothing> f) {
        f.discard();
        return Nothing();
      });

    Clock::advance(Milliseconds(1));
    Clock::settle();

    AWAIT_READY(future);
  }

  EXPECT_NONE(weak_future.get());
  EXPECT_FALSE(future.hasDiscard());
}


static Future<bool> readyFuture()
{
  return true;
}


static Future<bool> failedFuture()
{
  return Failure("The value is not positive (or zero)");
}


static Future<bool> pendingFuture(const Future<bool>& future)
{
  return future; // Keep it pending.
}


static Future<string> second(const bool& b)
{
  return b ? string("true") : string("false");
}


static Future<string> third(const string& s)
{
  return s;
}


TEST(FutureTest, Chain)
{
  Future<string> s = readyFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isReady());
  EXPECT_EQ("true", s.get());

  s = failedFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isFailed());

  Promise<bool> promise;

  s = pendingFuture(promise.future())
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  ASSERT_TRUE(s.isPending());

  promise.discard();

  AWAIT_DISCARDED(s);
}


static Future<bool> inner1(const Future<bool>& future)
{
  return future;
}


static Future<int> inner2(
    std::atomic_bool* executed, const Future<int>& future)
{
  executed->store(true);
  return future;
}


// Tests that Future::discard does not complete the future unless
// Promise::discard is invoked.
TEST(FutureTest, Discard1)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  std::atomic_bool executed(false);

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now discarding the promise should cause the outer future to be
  // discarded also.
  ASSERT_TRUE(promise1.discard());

  AWAIT_DISCARDED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed.load());
  ASSERT_TRUE(promise2.future().isPending());
}


// Tests that Future::discard does not complete the future and
// Promise::set can still be invoked to complete the future.
TEST(FutureTest, Discard2)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  std::atomic_bool executed(false);

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now setting the promise should cause the outer future to be
  // discarded rather than executing the last lambda because the
  // implementation of Future::then does not continue the chain once a
  // discard occurs.
  ASSERT_TRUE(promise1.set(true));

  AWAIT_DISCARDED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed.load());
  ASSERT_TRUE(promise2.future().isPending());
}


// Tests that Future::discard does not complete the future and
// Promise::fail can still be invoked to complete the future.
TEST(FutureTest, Discard3)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  std::atomic_bool executed(false);

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now failing the promise should cause the outer future to be
  // failed also.
  ASSERT_TRUE(promise1.fail("failure message"));

  AWAIT_FAILED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed.load());
  ASSERT_TRUE(promise2.future().isPending());
}


TEST(FutureTest, Select)
{
  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  std::set<Future<int>> futures = {
    promise1.future(),
    promise2.future(),
    promise3.future(),
    promise4.future()
  };

  promise1.set(42);

  Future<Future<int>> future = select(futures);

  AWAIT_READY(future);
  AWAIT_READY(future.get());
  EXPECT_EQ(42, future->get());

  futures.erase(promise1.future());

  future = select(futures);
  EXPECT_TRUE(future.isPending());

  future.discard();
  AWAIT_DISCARDED(future);
}


TEST(FutureTest, FromTry)
{
  Try<int> t = 1;
  Future<int> future = t;

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(1, future.get());

  t = Error("error");
  future = t;

  ASSERT_TRUE(future.isFailed());
}


TEST(FutureTest, FromTryFuture)
{
  Try<Future<int>> t = 1;
  Future<int> future = t;

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(1, future.get());

  Promise<int> p;
  t = p.future();
  future = t;

  ASSERT_TRUE(future.isPending());
  p.set(1);
  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(1, future.get());

  t = Error("error");
  future = t;

  ASSERT_TRUE(future.isFailed());
  EXPECT_EQ(t.error(), future.failure());
}


TEST(FutureTest, ArrowOperator)
{
  Future<string> s = string("hello");
  EXPECT_EQ(5u, s->size());
}


TEST(FutureTest, UndiscardableFuture)
{
  Promise<int> promise;

  Future<int> f = undiscardable(promise.future());

  f.discard();

  EXPECT_TRUE(f.hasDiscard());
  EXPECT_FALSE(promise.future().hasDiscard());

  promise.set(42);

  AWAIT_ASSERT_EQ(42, f);
}


TEST(FutureTest, UndiscardableLambda)
{
  Promise<int> promise;

  Future<int> f = Future<int>(2)
    .then(undiscardable([&](int multiplier) {
      return promise.future()
        .then([=](int i) {
          return i * multiplier;
        });
    }));

  f.discard();

  EXPECT_TRUE(f.hasDiscard());
  EXPECT_FALSE(promise.future().hasDiscard());

  promise.set(42);

  AWAIT_ASSERT_EQ(84, f);
}


TEST(FutureTest, Abandoned)
{
  AWAIT_EXPECT_ABANDONED(Future<int>());

  Owned<Promise<int>> promise(new Promise<int>());

  Future<int> future = promise->future();

  EXPECT_TRUE(!future.isAbandoned());

  promise.reset();

  AWAIT_EXPECT_ABANDONED(future);
}


TEST(FutureTest, AbandonedChain)
{
  Owned<Promise<int>> promise(new Promise<int>());

  Future<string> future = promise->future()
    .then([]() {
      return Nothing();
    })
    .then([]() -> string {
      return "hello world";
    });

  promise.reset();

  AWAIT_EXPECT_ABANDONED(future);
}


TEST(FutureTest, RecoverDiscarded)
{
  Promise<int> promise;

  Future<string> future = promise.future()
    .then([]() -> string {
      return "hello";
    })
    .recover([](const Future<string>&) -> string {
      return "world";
    });

  promise.discard();

  AWAIT_EQ("world", future);
}


TEST(FutureTest, RecoverFailed)
{
  Promise<int> promise;

  Future<string> future = promise.future()
    .then([]() -> string {
      return "hello";
    })
    .recover([](const Future<string>&) -> string {
      return "world";
    });

  promise.fail("Failure");

  AWAIT_EQ("world", future);
}


TEST(FutureTest, RecoverAbandoned)
{
  Owned<Promise<int>> promise(new Promise<int>());

  Future<string> future = promise->future()
    .then([]() -> string {
      return "hello";
    })
    .recover([](const Future<string>&) -> string {
      return "world";
    });

  promise.reset();

  AWAIT_EQ("world", future);
}


// Tests that we don't propagate a discard through a `recover()` but a
// discard can still be called and propagate later.
TEST(FutureTest, RecoverDiscard)
{
  Promise<int> promise1;
  Promise<string> promise2;
  Promise<string> promise3;
  Promise<string> promise4;

  Future<string> future = promise1.future()
    .then([]() -> string {
      return "hello";
    })
    .recover([&](const Future<string>&) {
      return promise2.future()
        .then([&]() {
          return promise3.future()
            .then([&]() {
              return promise4.future();
            });
        });
    });

  future.discard();

  promise1.discard();

  EXPECT_FALSE(promise2.future().hasDiscard());

  promise2.set(string("not world"));

  EXPECT_FALSE(promise3.future().hasDiscard());

  promise3.set(string("also not world"));

  EXPECT_FALSE(promise4.future().hasDiscard());

  future.discard();

  EXPECT_TRUE(promise4.future().hasDiscard());

  promise4.set(string("world"));

  AWAIT_EQ("world", future);
}
