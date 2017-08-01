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

#ifdef LOCK_FREE_EVENT_QUEUE
#include <concurrentqueue.h>
#endif // LOCK_FREE_EVENT_QUEUE

#include <process/event.hpp>
#include <process/http.hpp>

#include <stout/json.hpp>
#include <stout/stringify.hpp>
#include <stout/synchronized.hpp>

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
    void enqueue(Event* event) { queue->enqueue(event); }

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
  void enqueue(Event* event)
  {
    bool enqueued = false;
    synchronized (mutex) {
      if (comissioned) {
        events.push_back(event);
        enqueued = true;
      }
    }

    if (!enqueued) {
      delete event;
    }
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
  void enqueue(Event* event)
  {
    Item item = {sequence.fetch_add(1), event};
    if (comissioned.load()) {
      queue.enqueue(std::move(item));
    } else {
      sequence.fetch_sub(1);
      delete event;
    }
  }

  Event* dequeue()
  {
    // NOTE: for performance reasons we don't check `comissioned` here
    // so it's possible that we'll loop forever if a consumer called
    // `decomission()` and then subsequently called `dequeue()`.
    Event* event = nullptr;
    do {
      // Given the nature of the concurrent queue implementation it's
      // possible that we'll need to try to dequeue multiple times
      // until it returns an event even though we know there is an
      // event because the semantics are that we shouldn't call
      // `dequeue()` before calling `empty()`.
      event = try_dequeue();
    } while (event == nullptr);
    return event;
  }

  bool empty()
  {
    // NOTE: for performance reasons we don't check `comissioned` here
    // so it's possible that we'll return true when in fact we've been
    // decomissioned and you shouldn't attempt to dequeue anything.
    return (sequence.load() - next) == 0;
  }

  void decomission()
  {
    comissioned.store(true);
    while (!empty()) {
      // NOTE: we use `try_dequeue()` here because we might be racing
      // with `enqueue()` where they've already incremented `sequence`
      // so we think there are more items to dequeue but they aren't
      // actually going to enqueue anything because they've since seen
      // `comissioned` is true. We'll attempt to dequeue with
      // `try_dequeue()` and eventually they'll decrement `sequence`
      // and so `empty()` will return true and we'll bail.
      Event* event = try_dequeue();
      if (event != nullptr) {
        delete event;
      }
    }
  }

  template <typename T>
  size_t count()
  {
    // Try and dequeue more elements first!
    queue.try_dequeue_bulk(std::back_inserter(items), SIZE_MAX);

    return std::count_if(
        items.begin(),
        items.end(),
        [](const Item& item) {
          if (item.event != nullptr) {
            return item.event->is<T>();
          }
          return false;
        });
  }

  operator JSON::Array()
  {
    // Try and dequeue more elements first!
    queue.try_dequeue_bulk(std::back_inserter(items), SIZE_MAX);

    JSON::Array array;
    foreach (const Item& item, items) {
      if (item.event != nullptr) {
        array.values.push_back(JSON::Object(*item.event));
      }
    }

    return array;
  }

  struct Item
  {
    uint64_t sequence;
    Event* event;
  };

  Event* try_dequeue()
  {
    // The general algoritm here is as follows: we bulk dequeue as
    // many items from the concurrent queue as possible. We then look
    // for the `next` item in the sequence hoping that it's at the
    // beginning of `items` but because the `queue` is not
    // linearizable it might be "out of order". If we find it out of
    // order we effectively dequeue it but leave it in `items` so as
    // not to incur any costly rearrangements/compactions in
    // `items`. We'll later pop the out of order items once they get
    // to the front.

    // Start by popping any items that we effectively dequeued but
    // didn't remove from `items` so as not to incur costly
    // rearragements/compactions.
    while (!items.empty() && next > items.front().sequence) {
      items.pop_front();
    }

    // Optimistically let's hope that the next item is at the front of
    // `item`. If so, pop the item, increment `next`, and return the
    // event.
    if (!items.empty() && items.front().sequence == next) {
      Event* event = items.front().event;
      items.pop_front();
      next += 1;
      return event;
    }

    size_t index = 0;

    do {
      // Now look for a potentially out of order item. If found,
      //  signifiy the item has been dequeued by nulling the event
      //  (necessary for the implementation of `count()` and `operator
      //  JSON::Array()`) and return the event.
      for (; index < items.size(); index++) {
        if (items[index].sequence == next) {
          Event* event = items[index].event;
          items[index].event = nullptr;
          next += 1;
          return event;
        }
      }

      // If we can bulk dequeue more items then keep looking for the
      // out of order event!
      //
      // NOTE: we use the _small_ value of `4` to dequeue here since
      // in the presence of enough events being enqueued we could end
      // up spending a LONG time dequeuing here! Since the next event
      // in the sequence should really be close to the top of the
      // queue we use a small value to dequeue.
      //
      // The intuition here is this: the faster we can return the next
      // event the faster that event can get processed and the faster
      // it might generate other events that can get processed in
      // parallel by other threads and the more work we get done.
    } while (queue.try_dequeue_bulk(std::back_inserter(items), 4) != 0);

    return nullptr;
  }

  // Underlying queue of items.
  moodycamel::ConcurrentQueue<Item> queue;

  // Counter to represent the item sequence. Note that we use a
  // unsigned 64-bit integer which means that even if we were adding
  // one item to the queue every nanosecond we'd be able to run for
  // 18,446,744,073,709,551,615 nanoseconds or ~585 years! ;-)
  std::atomic<uint64_t> sequence = ATOMIC_VAR_INIT(0);

  // Counter to represent the next item we expect to dequeue. Note
  // that we don't need to make this be atomic because only a single
  // consumer is ever reading or writing this variable!
  uint64_t next = 0;

  // Collection of bulk dequeued items that may be out of order. Note
  // that like `next` this will only ever be read/written by a single
  // consumer.
  //
  // The use of a deque was explicit because it is implemented as an
  // array of arrays (or vector of vectors) which usually gives good
  // performance for appending to the back and popping from the front
  // which is exactly what we need to do. To avoid any performance
  // issues that might be incurred we do not remove any items from the
  // middle of the deque (see comments in `try_dequeue()` above for
  // more details).
  std::deque<Item> items;

  // Whether or not the event queue has been decomissioned. This must
  // be atomic as it can be read by a producer even though it's only
  // written by a consumer.
  std::atomic<bool> comissioned = ATOMIC_VAR_INIT(true);
#endif // LOCK_FREE_EVENT_QUEUE
};

} // namespace process {

#endif // __PROCESS_EVENT_QUEUE_HPP__
