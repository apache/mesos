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

#ifndef __PROCESS_MUTEX_HPP__
#define __PROCESS_MUTEX_HPP__

#include <atomic>
#include <memory>
#include <queue>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>
#include <stout/synchronized.hpp>

namespace process {

class Mutex
{
public:
  Mutex() : data(new Data()) {}

  Future<Nothing> lock()
  {
    Future<Nothing> future = Nothing();

    synchronized (data->lock) {
      if (!data->locked) {
        data->locked = true;
      } else {
        Promise<Nothing> waiter;
        future = waiter.future();
        data->waiters.push(std::move(waiter));
      }
    }

    return future;
  }

  void unlock()
  {
    // NOTE: We need to grab the promise 'date->promises.front()' but
    // set it outside of the critical section because setting it might
    // trigger callbacks that try to reacquire the lock.
    Option<Promise<Nothing>> waiter;

    synchronized (data->lock) {
      if (!data->waiters.empty()) {
        // TODO(benh): Skip a future that has been discarded?
        waiter = std::move(data->waiters.front());
        data->waiters.pop();
      } else {
        data->locked = false;
      }
    }

    if (waiter.isSome()) {
      waiter->set(Nothing());
    }
  }

private:
  struct Data
  {
    Data() : locked(false) {}

    ~Data()
    {
      // TODO(benh): Fail promises?
    }

    // Rather than use a process to serialize access to the mutex's
    // internal data we use a 'std::atomic_flag'.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;

    // Describes the state of this mutex.
    bool locked;

    // Represents "waiters" for this lock.
    std::queue<Promise<Nothing>> waiters;
  };

  std::shared_ptr<Data> data;
};

} // namespace process {

#endif // __PROCESS_MUTEX_HPP__
