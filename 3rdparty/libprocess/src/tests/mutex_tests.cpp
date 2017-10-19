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

#include <process/future.hpp>
#include <process/mutex.hpp>

using process::Future;
using process::Mutex;

TEST(MutexTest, Lock)
{
  Mutex mutex;

  // We should be able to acquire the mutex immediately.
  EXPECT_TRUE(mutex.lock().isReady());

  // After unlocking, we should be able to acquire the mutex
  // immediately again.
  mutex.unlock();
  EXPECT_TRUE(mutex.lock().isReady());
}


TEST(MutexTest, Block)
{
  Mutex mutex;

  // We should be able to acquire the mutex immediately.
  EXPECT_TRUE(mutex.lock().isReady());

  // Subsequent calls should "block".
  Future<Nothing> locked = mutex.lock();

  EXPECT_TRUE(locked.isPending());

  // After we release the mutex the future should be satisfied.
  mutex.unlock();

  EXPECT_TRUE(locked.isReady());
}


TEST(MutexTest, Queue)
{
  Mutex mutex;

  // We should be able to acquire the mutex immediately.
  EXPECT_TRUE(mutex.lock().isReady());

  // Subsequent calls should "block" and get queued.
  Future<Nothing> locked1 = mutex.lock();
  Future<Nothing> locked2 = mutex.lock();

  EXPECT_TRUE(locked1.isPending());
  EXPECT_TRUE(locked2.isPending());

  // After we release the mutex the first future should be satisfied.
  mutex.unlock();

  EXPECT_TRUE(locked1.isReady());
  EXPECT_TRUE(locked2.isPending());

  // And another release should satisfy the second future.
  mutex.unlock();

  EXPECT_TRUE(locked2.isReady());
}
