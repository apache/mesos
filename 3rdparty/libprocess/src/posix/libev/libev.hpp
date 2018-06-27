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

#ifndef __LIBEV_HPP__
#define __LIBEV_HPP__

#include <ev.h>

#include <mutex>
#include <queue>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/lambda.hpp>
#include <stout/synchronized.hpp>

namespace process {

// Event loop.
extern struct ev_loop* loop;

// Asynchronous watcher for interrupting loop to specifically deal
// with IO watchers and functions (via run_in_event_loop).
extern ev_async async_watcher;

// Queue of I/O watchers to be asynchronously added to the event loop
// (protected by 'watchers' below).
// TODO(benh): Replace this queue with functions that we put in
// 'functions' below that perform the ev_io_start themselves.
extern std::queue<ev_io*>* watchers;
extern std::mutex* watchers_mutex;

// Queue of functions to be invoked asynchronously within the vent
// loop (protected by 'watchers' above).
extern std::queue<lambda::function<void()>>* functions;

// Per thread bool pointer. We use a pointer to lazily construct the
// actual bool.
extern thread_local bool* _in_event_loop_;

#define __in_event_loop__ *(_in_event_loop_ == nullptr ?                \
  _in_event_loop_ = new bool(false) : _in_event_loop_)


// Wrapper around function we want to run in the event loop.
template <typename T>
void _run_in_event_loop(
    const lambda::function<Future<T>()>& f,
    const Owned<Promise<T>>& promise)
{
  // Don't bother running the function if the future has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
  } else {
    promise->set(f());
  }
}


// Helper for running a function in the event loop.
template <typename T>
Future<T> run_in_event_loop(const lambda::function<Future<T>()>& f)
{
  // If this is already the event loop then just run the function.
  if (__in_event_loop__) {
    return f();
  }

  Owned<Promise<T>> promise(new Promise<T>());

  Future<T> future = promise->future();

  // Enqueue the function.
  synchronized (watchers_mutex) {
    functions->push(lambda::bind(&_run_in_event_loop<T>, f, promise));
  }

  // Interrupt the loop.
  ev_async_send(loop, &async_watcher);

  return future;
}

} // namespace process {

#endif // __LIBEV_HPP__
