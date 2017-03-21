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

#include <gmock/gmock.h>

#include <atomic>
#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/loop.hpp>
#include <process/queue.hpp>

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Future;
using process::loop;
using process::Promise;
using process::Queue;

using std::string;


TEST(LoopTest, Sync)
{
  std::atomic_int value = ATOMIC_VAR_INIT(1);

  Future<Nothing> future = loop(
      [&]() {
        return value.load();
      },
      [](int i) -> ControlFlow<Nothing> {
        if (i != 0) {
          return Continue();
        }
        return Break();
      });

  EXPECT_TRUE(future.isPending());

  value.store(0);

  AWAIT_READY(future);
}


TEST(LoopTest, Async)
{
  Queue<int> queue;

  Promise<int> promise1;
  Promise<string> promise2;

  Future<string> future = loop(
      [&]() {
        return queue.get();
      },
      [&](int i) {
        promise1.set(i);
        return promise2.future()
          .then([](const string& s) -> ControlFlow<string> {
            return Break(s);
          });
      });

  EXPECT_TRUE(future.isPending());

  queue.put(1);

  AWAIT_EQ(1, promise1.future());

  EXPECT_TRUE(future.isPending());

  string s = "Hello world!";

  promise2.set(s);

  AWAIT_EQ(s, future);
}


TEST(LoopTest, DiscardIterate)
{
  Promise<int> promise;

  promise.future().onDiscard([&]() { promise.discard(); });

  Future<Nothing> future = loop(
      [&]() {
        return promise.future();
      },
      [&](int i) -> ControlFlow<Nothing> {
        return Break();
      });

  EXPECT_TRUE(future.isPending());

  future.discard();

  AWAIT_DISCARDED(future);
  EXPECT_TRUE(promise.future().hasDiscard());
}


TEST(LoopTest, DiscardBody)
{
  Promise<Nothing> promise;

  promise.future().onDiscard([&]() { promise.discard(); });

  Future<Nothing> future = loop(
      [&]() {
        return 42;
      },
      [&](int i) {
        return promise.future()
          .then([]() -> ControlFlow<Nothing> {
            return Break();
          });
      });

  EXPECT_TRUE(future.isPending());

  future.discard();

  AWAIT_DISCARDED(future);
  EXPECT_TRUE(promise.future().hasDiscard());
}
