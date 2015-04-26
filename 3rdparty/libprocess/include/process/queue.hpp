#ifndef __PROCESS_QUEUE_HPP__
#define __PROCESS_QUEUE_HPP__

#include <deque>
#include <memory>
#include <queue>

#include <process/future.hpp>
#include <process/internal.hpp>
#include <process/owned.hpp>

namespace process {

template <typename T>
class Queue
{
public:
  Queue() : data(new Data()) {}

  void put(const T& t)
  {
    // NOTE: We need to grab the promise 'date->promises.front()' but
    // set it outside of the critical section because setting it might
    // trigger callbacks that try to reacquire the lock.
    Owned<Promise<T>> promise;

    internal::acquire(&data->lock);
    {
      if (data->promises.empty()) {
        data->elements.push(t);
      } else {
        promise = data->promises.front();
        data->promises.pop_front();
      }
    }
    internal::release(&data->lock);

    if (promise.get() != NULL) {
      promise->set(t);
    }
  }

  Future<T> get()
  {
    Future<T> future;

    internal::acquire(&data->lock);
    {
      if (data->elements.empty()) {
        data->promises.push_back(Owned<Promise<T>>(new Promise<T>()));
        future = data->promises.back()->future();
      } else {
        future = Future<T>(data->elements.front());
        data->elements.pop();
      }
    }
    internal::release(&data->lock);

    return future;
  }

private:
  struct Data
  {
    Data() : lock(0) {}

    ~Data()
    {
      // TODO(benh): Fail promises?
    }

    // Rather than use a process to serialize access to the queue's
    // internal data we use a low-level "lock" which we acquire and
    // release using atomic builtins.
    int lock;

    // Represents "waiters" for elements from the queue.
    std::deque<Owned<Promise<T>>> promises;

    // Represents elements already put in the queue.
    std::queue<T> elements;
  };

  std::shared_ptr<Data> data;
};

} // namespace process {

#endif // __PROCESS_QUEUE_HPP__
