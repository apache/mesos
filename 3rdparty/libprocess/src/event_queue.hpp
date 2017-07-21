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

namespace process {

// TODO(benh): Document that the contract/semantics that an event
// queue is designed to be _multiple_ producers and _single_ consumer
// and that currently that requirement is not enforced or even checked
// at runtime.

class EventQueue
{
public:
  void enqueue(Event* event)
  {
    bool enqueued = false;
    synchronized (mutex) {
      if (!decomissioned) {
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
    synchronized (mutex) {
      if (events.size() > 0) {
        Event* event = events.front();
        events.pop_front();
        return event;
      }
    }
    // TODO(benh): The current semantics are that a call to dequeue
    // will always be coupled by a call to empty and only get called
    // if we're not empty which implies we should never return nullptr
    // here, should we check this?
    return nullptr;
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
      decomissioned = true;
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
        JSON::Object object = *event;
        array.values.push_back(object);
      }
    }
    return array;
  }

private:
  std::mutex mutex;
  std::deque<Event*> events;
  bool decomissioned = false;
};

} // namespace process {

#endif // __PROCESS_EVENT_QUEUE_HPP__
