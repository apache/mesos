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

#ifndef __PROCESS_RWMUTEX_HPP__
#define __PROCESS_RWMUTEX_HPP__

#include <atomic>
#include <memory>
#include <queue>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/synchronized.hpp>

namespace process {

/**
 * ReadWriteLock is a lock that allows concurrent reads and
 * exclusive writes.
 *
 * To prevent starvation of write lock requests, reads will
 * queue when one or more write lock requests is waiting, even
 * if the read lock is currently acquired.
 */
class ReadWriteLock
{
public:
  ReadWriteLock() : data(new Data()) {}

  // TODO(bmahler): Consider returning a 'Locked' object in the
  // future as the mechanism for unlocking, rather than exposing
  // unlocking functions for all to call.
  Future<Nothing> write_lock()
  {
    Future<Nothing> future = Nothing();

    synchronized (data->lock) {
      if (!data->write_locked && data->read_locked == 0u) {
        data->write_locked = true;
      } else {
        Waiter w{Waiter::WRITE};
        future = w.promise.future();
        data->waiters.push(std::move(w));
      }
    }

    return future;
  }

  void write_unlock()
  {
    // NOTE: We need to satisfy the waiter(s) futures outside the
    // critical section because it might trigger callbacks which
    // try to reacquire a read or write lock.
    std::queue<Waiter> unblocked;

    synchronized (data->lock) {
      CHECK(data->write_locked);
      CHECK_EQ(data->read_locked, 0u);

      data->write_locked = false;

      if (!data->waiters.empty()) {
        switch (data->waiters.front().type) {
          case Waiter::READ:
            // Dequeue the group of readers at the front.
            while (!data->waiters.empty() &&
                   data->waiters.front().type == Waiter::READ) {
              unblocked.push(std::move(data->waiters.front()));
              data->waiters.pop();
            }

            data->read_locked = unblocked.size();

            break;

          case Waiter::WRITE:
            unblocked.push(std::move(data->waiters.front()));
            data->waiters.pop();
            data->write_locked = true;

            CHECK_EQ(data->read_locked, 0u);

            break;
          }
      }
    }

    while (!unblocked.empty()) {
      unblocked.front().promise.set(Nothing());
      unblocked.pop();
    }
  }

  // TODO(bmahler): Consider returning a 'Locked' object in the
  // future as the mechanism for unlocking, rather than exposing
  // unlocking functions for all to call.
  Future<Nothing> read_lock()
  {
    Future<Nothing> future = Nothing();

    synchronized (data->lock) {
      if (!data->write_locked && data->waiters.empty()) {
        data->read_locked++;
      } else {
        Waiter w{Waiter::READ};
        future = w.promise.future();
        data->waiters.push(std::move(w));
      }
    }

    return future;
  }

  void read_unlock()
  {
    // NOTE: We need to satisfy the waiter future outside the
    // critical section because it might trigger callbacks which
    // try to reacquire a read or write lock.
    Option<Waiter> waiter;

    synchronized (data->lock) {
      CHECK(!data->write_locked);
      CHECK_GT(data->read_locked, 0u);

      data->read_locked--;

      if (data->read_locked == 0u && !data->waiters.empty()) {
        CHECK_EQ(data->waiters.front().type, Waiter::WRITE);

        waiter = std::move(data->waiters.front());
        data->waiters.pop();
        data->write_locked = true;
      }
    }

    if (waiter.isSome()) {
      waiter->promise.set(Nothing());
    }
  }

private:
  struct Waiter
  {
    enum { READ, WRITE } type;
    Promise<Nothing> promise;
  };

  struct Data
  {
    Data() : read_locked(0), write_locked(false) {}

    ~Data()
    {
      // TODO(zhitao): Fail promises?
    }

    // The state of the lock can be either:
    //   (1) Unlocked: an incoming read or write grabs the lock.
    //
    //   (2) Read locked (by one or more readers): an incoming write
    //       will queue in the waiters. An incoming read will proceed
    //       if no one is waiting, otherwise it will queue.
    //
    //   (3) Write locked: incoming reads and writes will queue.

    size_t read_locked;
    bool write_locked;
    std::queue<Waiter> waiters;

    // Rather than use a process to serialize access to the
    // internal data we use a 'std::atomic_flag'.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
  };

  std::shared_ptr<Data> data;
};

} // namespace process {

#endif // __PROCESS_RWMUTEX_HPP__
