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

#include <string>

#include <process/collect.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/stringify.hpp>

using process::Future;
using process::Owned;
using process::Promise;

using std::string;
using std::vector;

TEST(CollectTest, Ready)
{
  // First ensure an empty list functions correctly.
  vector<Future<int>> empty;
  Future<vector<int>> collect = process::collect(empty);

  AWAIT_READY(collect);
  EXPECT_TRUE(collect->empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  vector<Future<int>> futures = {
    promise1.future(),
    promise2.future(),
    promise3.future(),
    promise4.future(),
  };

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  collect = process::collect(futures);

  AWAIT_ASSERT_READY(collect);

  vector<int> values = {1, 2, 3, 4};

  // We expect them to be returned in the same order as the
  // future list that was passed in.
  EXPECT_EQ(values, collect.get());
}


TEST(CollectTest, Failure)
{
  Promise<int> promise1;
  Promise<bool> promise2;

  Future<std::tuple<int, bool>> collect =
    process::collect(promise1.future(), promise2.future());

  ASSERT_TRUE(collect.isPending());

  promise1.set(42);

  ASSERT_TRUE(collect.isPending());

  promise2.set(true);

  AWAIT_READY(collect);

  std::tuple<int, bool> values = collect.get();

  ASSERT_EQ(42, std::get<0>(values));
  ASSERT_TRUE(std::get<1>(values));

  // Collect should fail when a future fails.
  Promise<bool> promise3;

  collect = process::collect(promise1.future(), promise3.future());

  ASSERT_TRUE(collect.isPending());

  promise3.fail("failure");

  AWAIT_FAILED(collect);

  // Collect should fail when a future is discarded.
  Promise<bool> promise4;

  collect = process::collect(promise1.future(), promise4.future());

  ASSERT_TRUE(collect.isPending());

  promise4.discard();

  AWAIT_FAILED(collect);
}


TEST(CollectTest, DiscardPropagation)
{
  Promise<int> promise1;
  Promise<bool> promise2;

  promise1.future()
    .onDiscard([&](){ promise1.discard(); });
  promise2.future()
    .onDiscard([&](){ promise2.discard(); });

  Future<std::tuple<int, bool>> collect = process::collect(
      promise1.future(),
      promise2.future());

  collect.discard();

  AWAIT_DISCARDED(collect);

  AWAIT_DISCARDED(promise1.future());
  AWAIT_DISCARDED(promise2.future());
}


TEST(CollectTest, AbandonedPropagation)
{
  Owned<Promise<int>> promise(new Promise<int>());

  // There is a race from the time that we reset the promise to when
  // the collect process is terminated so we need to use
  // Future::recover to properly handle this case.
  Future<int> future = process::collect(promise->future())
    .recover([](const Future<std::tuple<int>>& f) -> Future<std::tuple<int>> {
      if (f.isAbandoned()) {
        return std::make_tuple(42);
      }
      return f;
    })
    .then([](const std::tuple<int>& t) {
      return std::get<0>(t);
    });

  promise.reset();

  AWAIT_EQ(42, future);
}


TEST(AwaitTest, Success)
{
  // First ensure an empty list functions correctly.
  vector<Future<int>> empty;
  Future<vector<Future<int>>> future = process::await(empty);
  AWAIT_ASSERT_READY(future);
  EXPECT_TRUE(future->empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  vector<Future<int>> futures = {
    promise1.future(),
    promise2.future(),
    promise3.future(),
    promise4.future(),
  };

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  future = process::await(futures);

  AWAIT_ASSERT_READY(future);

  EXPECT_EQ(futures.size(), future->size());

  // We expect them to be returned in the same order as the
  // future list that was passed in.
  int i = 1;
  foreach (const Future<int>& result, future.get()) {
    ASSERT_TRUE(result.isReady());
    ASSERT_EQ(i, result.get());
    ++i;
  }
}


TEST(AwaitTest, Failure)
{
  Promise<int> promise1;
  Promise<bool> promise2;

  Future<std::tuple<Future<int>, Future<bool>>> await =
    process::await(promise1.future(), promise2.future());

  ASSERT_TRUE(await.isPending());

  promise1.set(42);

  ASSERT_TRUE(await.isPending());

  promise2.fail("failure message");

  AWAIT_READY(await);

  std::tuple<Future<int>, Future<bool>> futures = await.get();

  ASSERT_TRUE(std::get<0>(futures).isReady());
  ASSERT_EQ(42, std::get<0>(futures).get());

  ASSERT_TRUE(std::get<1>(futures).isFailed());
}


TEST(AwaitTest, Discarded)
{
  Promise<int> promise1;
  Promise<bool> promise2;

  Future<std::tuple<Future<int>, Future<bool>>> await =
    process::await(promise1.future(), promise2.future());

  ASSERT_TRUE(await.isPending());

  promise1.set(42);

  ASSERT_TRUE(await.isPending());

  promise2.discard();

  AWAIT_READY(await);

  std::tuple<Future<int>, Future<bool>> futures = await.get();

  ASSERT_TRUE(std::get<0>(futures).isReady());
  ASSERT_EQ(42, std::get<0>(futures).get());

  ASSERT_TRUE(std::get<1>(futures).isDiscarded());
}


TEST(AwaitTest, DiscardPropagation)
{
  Promise<int> promise1;
  Promise<bool> promise2;

  promise1.future()
    .onDiscard([&](){ promise1.discard(); });
  promise2.future()
    .onDiscard([&](){ promise2.discard(); });

  Future<std::tuple<Future<int>, Future<bool>>> await = process::await(
      promise1.future(),
      promise2.future());

  await.discard();

  AWAIT_DISCARDED(await);

  AWAIT_DISCARDED(promise1.future());
  AWAIT_DISCARDED(promise2.future());
}


TEST(AwaitTest, AbandonedPropagation)
{
  Owned<Promise<int>> promise(new Promise<int>());

  // There is a race from the time that we reset the promise to when
  // the await process is terminated so we need to use
  // Future::recover to properly handle this case.
  Future<int> future = process::await(promise->future(), Future<int>())
    .recover([](const Future<std::tuple<Future<int>, Future<int>>>& f)
             -> Future<std::tuple<Future<int>, Future<int>>> {
      if (f.isAbandoned()) {
        return std::make_tuple(42, 0);
      }
      return f;
    })
    .then([](const std::tuple<Future<int>, Future<int>>& t) {
      return std::get<0>(t)
        .then([](int i) {
          return i;
        });
    });

  promise.reset();

  AWAIT_EQ(42, future);
}


TEST(AwaitTest, AwaitSingleDiscard)
{
  Promise<int> promise;

  auto bar = [&]() {
    return promise.future();
  };

  auto foo = [&]() {
    return await(bar())
      .then([](const Future<int>& f) {
        return f
          .then([](int i) {
            return stringify(i);
          });
      });
  };

  Future<string> future = foo();

  future.discard();

  AWAIT_DISCARDED(future);

  EXPECT_TRUE(promise.future().hasDiscard());
}


TEST(AwaitTest, AwaitSingleAbandon)
{
  Owned<Promise<int>> promise(new Promise<int>());

  auto bar = [&]() {
    return promise->future();
  };

  auto foo = [&]() {
    return await(bar())
      .then([](const Future<int>& f) {
        return f
          .then([](int i) {
            return stringify(i);
          });
      });
  };

  // There is a race from the time that we reset the promise to when
  // the await process is terminated so we need to use Future::recover
  // to properly handle this case.
  Future<string> future = foo()
    .recover([](const Future<string>& f) -> Future<string> {
      if (f.isAbandoned()) {
        return "hello";
      }
      return f;
    });

  promise.reset();

  AWAIT_EQ("hello", future);
}
