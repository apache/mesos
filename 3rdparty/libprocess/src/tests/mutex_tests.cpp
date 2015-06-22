#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/mutex.hpp>

using namespace process;

TEST(Mutex, Lock)
{
  Mutex mutex;

  // We should be able to acquire the mutex immediately.
  EXPECT_TRUE(mutex.lock().isReady());

  // After unlocking, we should be able to acquire the mutex
  // immediately again.
  mutex.unlock();
  EXPECT_TRUE(mutex.lock().isReady());
}


TEST(Mutex, Block)
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


TEST(Mutex, Queue)
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
