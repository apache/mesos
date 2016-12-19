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

#include <process/after.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/loop.hpp>

#include <stout/duration.hpp>

using process::after;
using process::Break;
using process::Clock;
using process::Continue;
using process::ControlFlow;
using process::Future;
using process::loop;
using process::Promise;


TEST(AfterTest, After)
{
  AWAIT_READY(after(Milliseconds(1)));

  Clock::pause();

  Future<Nothing> future = after(Days(1));

  EXPECT_TRUE(future.isPending());

  Clock::advance(Days(1));

  AWAIT_READY(future);

  future = after(Days(1));

  EXPECT_TRUE(future.isPending());

  future.discard();

  AWAIT_DISCARDED(future);

  Clock::resume();
}


TEST(AfterTest, Loop)
{
  Future<Nothing> future =
    loop(None(),
         []() {
           return after(Days(1));
         },
         [](const Nothing&) -> ControlFlow<Nothing> {
           return Continue();
         });

  EXPECT_TRUE(future.isPending());

  future.discard();

  AWAIT_DISCARDED(future);

  Promise<Nothing> promise1;
  Promise<ControlFlow<Nothing>> promise2;

  future =
    loop(None(),
         []() {
           return after(Milliseconds(1));
         },
         [&](const Nothing&) {
           promise1.set(Nothing());
           return promise2.future();
         });

  EXPECT_TRUE(future.isPending());

  AWAIT_READY(promise1.future());

  promise2.future().onDiscard([&]() {
    promise2.set(ControlFlow<Nothing>(Break()));
  });

  future.discard();

  AWAIT_READY(future);
}
