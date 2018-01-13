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

#ifndef __PROCESS_QUEUE_HPP__
#define __PROCESS_QUEUE_HPP__

#include <atomic>
#include <deque>
#include <memory>
#include <queue>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/synchronized.hpp>

namespace process {

template <typename T>
class Queue
{
public:
  Queue() : data(new Data()) {}

  // TODO(bmahler): Take a T&& here instead.
  void put(const T& t)
  {
    // NOTE: We need to grab the promise 'date->promises.front()' but
    // set it outside of the critical section because setting it might
    // trigger callbacks that try to reacquire the lock.
    Owned<Promise<T>> promise;

    synchronized (data->lock) {
      if (data->promises.empty()) {
        data->elements.push(t);
      } else {
        promise = data->promises.front();
        data->promises.pop_front();
      }
    }

    if (promise.get() != nullptr) {
      promise->set(t);
    }
  }

  Future<T> get()
  {
    Future<T> future;

    synchronized (data->lock) {
      if (data->elements.empty()) {
        data->promises.emplace_back(new Promise<T>());
        future = data->promises.back()->future();
      } else {
        T t = std::move(data->elements.front());
        data->elements.pop();
        return Future<T>(std::move(t));
      }
    }

    // If there were no items available, we set up a discard
    // handler. This is done here to minimize the amount of
    // work done within the critical section above.
    auto weak_data = std::weak_ptr<Data>(data);

    future.onDiscard([weak_data, future]() {
      auto data = weak_data.lock();
      if (!data) {
        return;
      }

      synchronized (data->lock) {
        for (auto it = data->promises.begin();
             it != data->promises.end();
             ++it) {
          if ((*it)->future() == future) {
            (*it)->discard();
            data->promises.erase(it);
            break;
          }
        }
      }
    });

    return future;
  }

  size_t size() const
  {
    synchronized (data->lock) {
      return data->elements.size();
    }
  }

private:
  struct Data
  {
    Data() = default;

    ~Data()
    {
      // TODO(benh): Fail promises?
    }

    // Rather than use a process to serialize access to the queue's
    // internal data we use a 'std::atomic_flag'.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;

    // Represents "waiters" for elements from the queue.
    std::deque<Owned<Promise<T>>> promises;

    // Represents elements already put in the queue.
    std::queue<T> elements;
  };

  std::shared_ptr<Data> data;
};

} // namespace process {

#endif // __PROCESS_QUEUE_HPP__
