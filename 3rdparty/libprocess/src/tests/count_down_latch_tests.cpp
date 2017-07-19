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

#include <thread>
#include <vector>

#include <process/count_down_latch.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>

using process::CountDownLatch;
using process::Future;


TEST(CountDownLatchTest, Triggered)
{
  CountDownLatch latch(5);

  Future<Nothing> triggered = latch.triggered();

  latch.decrement();

  EXPECT_TRUE(triggered.isPending());

  latch.decrement();

  EXPECT_TRUE(triggered.isPending());

  latch.decrement();

  EXPECT_TRUE(triggered.isPending());

  latch.decrement();

  EXPECT_TRUE(triggered.isPending());

  latch.decrement();

  AWAIT_READY(triggered);
}


TEST(CountDownLatchTest, Threads)
{
  CountDownLatch latch(5);

  std::vector<std::thread> threads;

  for (size_t i = 0; i < 5; i++) {
    threads.emplace_back(
        [&]() {
          latch.decrement();
        });
  }

  foreach (std::thread& thread, threads) {
    thread.join();
  }

  AWAIT_READY(latch.triggered());
}
