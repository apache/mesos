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

#ifndef __PROCESS_EVENT_QUEUE_HPP__
#define __PROCESS_EVENT_QUEUE_HPP__

#include <deque>
#include <mutex>
#include <string>

#include <process/event.hpp>
#include <process/http.hpp>

#include <stout/json.hpp>
#include <stout/stringify.hpp>
#include <stout/synchronized.hpp>

#ifdef LOCK_FREE_EVENT_QUEUE
#include "mpsc_linked_queue.hpp"
#endif // LOCK_FREE_EVENT_QUEUE

namespace process {

// A _multiple_ producer (MP) _single_ consumer (SC) event queue for a
// process. Note that we don't _enforce_ the MP/SC semantics during
// runtime but we have explicitly separated out the `Producer`
// interface and the `Consumer` interface in order to help avoid
// incorrect usage.
//
// Notable semantics:
//
//   * Consumers _must_ call `empty()` before calling
//     `dequeue()`. Failing to do so may result in undefined behavior.
//
//   * After a consumer calls `decomission()` they _must_ not call any
//     thing else (not even `empty()` and especially not
//     `dequeue()`). Doing so is undefined behavior.
//
// Notes on the lock-free implementation:
//
// The SC requirement is necessary for the lock-free implementation
// because the underlying queue does not provide linearizability which
// means events can be dequeued "out of order". Usually this is not a
// problem, after all, in most circumstances we won't know the order
// in which events might be enqueued in the first place. However, this
// can be a very bad problem if a single process attempts to enqueue
// two events in a different process AND THOSE EVENTS ARE
// REORDERED. To ensure this will never be the case we give every
// event a sequence number. That way an event from the same process
// will always have a happens-before relationship with respect to the
// events that they enqueue because they'll have distinct sequence
// numbers.
//
// This makes the consumer implementation more difficult because the
// consumer might need to "reorder" events as it reads them out. To do
// this efficiently we require only a single consumer, which fits well
// into the actor model because there will only ever be a single
// thread consuming an actors events at a time.
class EventQueue
{
public:
  EventQueue() : producer(this), consumer(this) {}

  class Producer
  {
  public:
    // Returns false if not enqueued; this means the queue
    // is decomissioned. In this case the caller retains
    // ownership of the event.
    bool enqueue(Event* event) { return queue->enqueue(event); }

  private:
    friend class EventQueue;

    Producer(EventQueue* queue) : queue(queue) {}

    EventQueue* queue;
  } producer;

  class Consumer
  {
  public:
    Event* dequeue() { return queue->dequeue(); }
    bool empty() { return queue->empty(); }
    void decomission() { queue->decomission(); }
    template <typename T>
    size_t count() { return queue->count<T>(); }
    operator JSON::Array() { return queue->operator JSON::Array(); }

  private:
    friend class EventQueue;

    Consumer(EventQueue* queue) : queue(queue) {}

    EventQueue* queue;
  } consumer;

private:
  friend class Producer;
  friend class Consumer;

#ifndef LOCK_FREE_EVENT_QUEUE
  bool enqueue(Event* event)
  {
    synchronized (mutex) {
      if (comissioned) {
        events.push_back(event);
        return true;
      }
    }

    return false;
  }

  Event* dequeue()
  {
    Event* event = nullptr;

    synchronized (mutex) {
      if (events.size() > 0) {
        Event* event = events.front();
        events.pop_front();
        return event;
      }
    }

    // Semantics are the consumer _must_ call `empty()` before calling
    // `dequeue()` which means an event must be present.
    return CHECK_NOTNULL(event);
  }

  bool empty()
  {
    synchronized (mutex) {
      return events.size() == 0;
    }
  }

  void decomission()
  {
    synchronized (mutex) {
      comissioned = false;
      while (!events.empty()) {
        Event* event = events.front();
        events.pop_front();
        delete event;
      }
    }
  }

  template <typename T>
  size_t count()
  {
    synchronized (mutex) {
      return std::count_if(
          events.begin(),
          events.end(),
          [](const Event* event) {
            return event->is<T>();
          });
    }
  }

  operator JSON::Array()
  {
    JSON::Array array;
    synchronized (mutex) {
      foreach (Event* event, events) {
        array.values.push_back(JSON::Object(*event));
      }
    }
    return array;
  }

  std::mutex mutex;
  std::deque<Event*> events;
  bool comissioned = true;
#else // LOCK_FREE_EVENT_QUEUE
  bool enqueue(Event* event)
  {
    if (comissioned.load()) {
      queue.enqueue(event);
      return true;
    }

    return false;
  }

  Event* dequeue()
  {
    return queue.dequeue();
  }

  bool empty()
  {
    return queue.empty();
  }

  void decomission()
  {
    comissioned.store(true);
    while (!empty()) {
      delete dequeue();
    }
  }

  template <typename T>
  size_t count()
  {
    size_t count = 0;
    queue.for_each([&count](Event* event) {
      if (event->is<T>()) {
        count++;
      }
    });
    return count;
  }

  operator JSON::Array()
  {
    JSON::Array array;
    queue.for_each([&array](Event* event) {
      array.values.push_back(JSON::Object(*event));
    });

    return array;
  }

  // Underlying queue of items.
  MpscLinkedQueue<Event> queue;

  // Whether or not the event queue has been decomissioned. This must
  // be atomic as it can be read by a producer even though it's only
  // written by a consumer.
  std::atomic<bool> comissioned = ATOMIC_VAR_INIT(true);
#endif // LOCK_FREE_EVENT_QUEUE
};

} // namespace process {

#endif // __PROCESS_EVENT_QUEUE_HPP__
