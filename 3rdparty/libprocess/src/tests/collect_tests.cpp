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

#include <process/collect.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>

using process::Future;
using process::Promise;

using std::list;

TEST(CollectTest, Ready)
{
  // First ensure an empty list functions correctly.
  list<Future<int>> empty;
  Future<list<int>> collect = process::collect(empty);

  AWAIT_READY(collect);
  EXPECT_TRUE(collect.get().empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  list<Future<int>> futures;
  futures.push_back(promise1.future());
  futures.push_back(promise2.future());
  futures.push_back(promise3.future());
  futures.push_back(promise4.future());

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  collect = process::collect(futures);

  AWAIT_ASSERT_READY(collect);

  list<int> values;
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  values.push_back(4);

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
  Future<int> future1;
  Future<bool> future2;

  future1
    .onDiscard([=](){ process::internal::discarded(future1); });
  future2
    .onDiscard([=](){ process::internal::discarded(future2); });

  Future<std::tuple<int, bool>> collect = process::collect(future1, future2);

  collect.discard();

  AWAIT_DISCARDED(future1);
  AWAIT_DISCARDED(future2);
}


TEST(AwaitTest, Success)
{
  // First ensure an empty list functions correctly.
  list<Future<int>> empty;
  Future<list<Future<int>>> future = process::await(empty);
  AWAIT_ASSERT_READY(future);
  EXPECT_TRUE(future.get().empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  list<Future<int>> futures;
  futures.push_back(promise1.future());
  futures.push_back(promise2.future());
  futures.push_back(promise3.future());
  futures.push_back(promise4.future());

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  future = process::await(futures);

  AWAIT_ASSERT_READY(future);

  EXPECT_EQ(futures.size(), future.get().size());

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
  Future<int> future1;
  Future<bool> future2;

  future1
    .onDiscard([=](){ process::internal::discarded(future1); });
  future2
    .onDiscard([=](){ process::internal::discarded(future2); });

  Future<std::tuple<Future<int>, Future<bool>>> await =
    process::await(future1, future2);

  await.discard();

  AWAIT_DISCARDED(future1);
  AWAIT_DISCARDED(future2);
}
