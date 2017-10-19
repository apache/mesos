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
#include <process/rwlock.hpp>

using process::Future;
using process::ReadWriteLock;


TEST(ReadWriteLockTest, LockWhenUnlocked)
{
  ReadWriteLock lock;

  // We should be able to lock for writing or reading when the
  // lock is unlocked.

  EXPECT_TRUE(lock.write_lock().isReady());
  lock.write_unlock();

  EXPECT_TRUE(lock.read_lock().isReady());
  lock.read_unlock();

  EXPECT_TRUE(lock.write_lock().isReady());
  lock.write_unlock();
}


TEST(ReadWriteLockTest, BlockReads)
{
  ReadWriteLock lock;

  // We should be able to acquire the write lock immediately.
  EXPECT_TRUE(lock.write_lock().isReady());

  // Subsequent reads should "block".
  Future<Nothing> read = lock.read_lock();
  EXPECT_TRUE(read.isPending());

  // After we release the write lock,
  // the read lock will be acquired.
  lock.write_unlock();

  EXPECT_TRUE(read.isReady());
  lock.read_unlock();
}


TEST(ReadWriteLockTest, BlockWrites)
{
  ReadWriteLock lock;

  // We should be able to acquire the write lock immediately.
  EXPECT_TRUE(lock.write_lock().isReady());

  // Subsequent writes should "block".
  Future<Nothing> write = lock.write_lock();
  EXPECT_TRUE(write.isPending());

  // After we release the write lock,
  // the next write lock will be acquired.
  lock.write_unlock();

  EXPECT_TRUE(write.isReady());
  lock.write_unlock();

  // The write lock should also "block" behind a read lock.
  EXPECT_TRUE(lock.read_lock().isReady());

  write = lock.write_lock();
  EXPECT_TRUE(write.isPending());

  // After we release the read lock,
  // the next write lock will be acquired.
  lock.read_unlock();

  EXPECT_TRUE(write.isReady());
  lock.write_unlock();
}


TEST(ReadWriteLockTest, ConcurrentReads)
{
  ReadWriteLock lock;

  // We should be able to acquire read lock multiple times.
  EXPECT_TRUE(lock.read_lock().isReady());
  EXPECT_TRUE(lock.read_lock().isReady());
  EXPECT_TRUE(lock.read_lock().isReady());

  // Subsequent writes should "block" and won't be unblocked
  // until all reads complete.
  Future<Nothing> write = lock.write_lock();
  EXPECT_TRUE(write.isPending());

  lock.read_unlock();
  EXPECT_TRUE(write.isPending());

  lock.read_unlock();
  EXPECT_TRUE(write.isPending());

  lock.read_unlock();
  EXPECT_TRUE(write.isReady());

  lock.write_unlock();
}


TEST(ReadWriteLockTest, NoStarvation)
{
  ReadWriteLock lock;

  // We should be able to acquire read lock immediately.
  EXPECT_TRUE(lock.read_lock().isReady());

  // Queue a [WRITE1, READ1, READ2, WRITE2, READ3, READ4].
  // The reads will run in groups, but cannot cross
  // the write boundaries (i.e. READ3 and READ4 must wait
  // behind WRITE2).

  Future<Nothing> write1 = lock.write_lock();
  EXPECT_TRUE(write1.isPending());

  Future<Nothing> read1 = lock.read_lock();
  EXPECT_TRUE(read1.isPending());

  Future<Nothing> read2 = lock.read_lock();
  EXPECT_TRUE(read2.isPending());

  Future<Nothing> write2 = lock.write_lock();
  EXPECT_TRUE(write2.isPending());

  Future<Nothing> read3 = lock.read_lock();
  EXPECT_TRUE(read3.isPending());

  Future<Nothing> read4 = lock.read_lock();
  EXPECT_TRUE(read4.isPending());

  // Release the intial read lock.
  lock.read_unlock();

  EXPECT_TRUE(write1.isReady());
  EXPECT_TRUE(read1.isPending());
  EXPECT_TRUE(read2.isPending());
  EXPECT_TRUE(write2.isPending());
  EXPECT_TRUE(read3.isPending());
  EXPECT_TRUE(read4.isPending());

  lock.write_unlock();

  EXPECT_TRUE(read1.isReady());
  EXPECT_TRUE(read2.isReady());
  EXPECT_TRUE(write2.isPending());
  EXPECT_TRUE(read3.isPending());
  EXPECT_TRUE(read4.isPending());

  lock.read_unlock();
  lock.read_unlock();

  EXPECT_TRUE(write2.isReady());
  EXPECT_TRUE(read3.isPending());
  EXPECT_TRUE(read4.isPending());

  lock.write_unlock();

  EXPECT_TRUE(read3.isReady());
  EXPECT_TRUE(read4.isReady());

  lock.read_unlock();
  lock.read_unlock();
}
