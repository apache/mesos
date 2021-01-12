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

// Array of event loops.
extern struct ev_loop** loops;

// Array of async watchers for interrupting loops to specifically deal
// with IO watchers and functions (via run_in_event_loop).
extern ev_async* async_watchers;

// Array of queues of functions to be invoked asynchronously within the
// event loops (each queue is protected by a mutex).
extern std::mutex* functions_mutexes;
extern std::queue<lambda::function<void()>>* functions;

// Per thread loop pointer. If this thread is currently inside an
// event loop, then this will be set to point to the loop that it's
// executing inside. Otherwise, will be set to null.
extern thread_local struct ev_loop* _in_event_loop_;

// This is a wrapper type of the loop index to ensure that
// `get_loop(fd)` is called to select the correct loop for
// `run_in_event_loop(...)`.
struct LoopIndex
{
  size_t index;

private:
  explicit LoopIndex(size_t index) : index(index) {}
  LoopIndex() = delete;
  friend LoopIndex get_loop(int_fd fd);
};


// Since multiple event loops are supported, and fds are assigned
// to loops, callers must first get the loop based on the fd.
LoopIndex get_loop(int_fd fd);


// Wrapper around function we want to run in the event loop.
template <typename T>
void _run_in_event_loop(
    struct ev_loop* loop,
    const lambda::function<Future<T>(struct ev_loop*)>& f,
    const Owned<Promise<T>>& promise)
{
  // Don't bother running the function if the future has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
  } else {
    promise->set(f(loop));
  }
}


// Helper for running a function in one of the event loops.
template <typename T>
Future<T> run_in_event_loop(
    const LoopIndex loop_index,
    const lambda::function<Future<T>(struct ev_loop*)>& f)
{
  struct ev_loop* loop = loops[loop_index.index];

  // If this is already the event loop that we're trying to run the
  // function within, then just run the function.
  if (_in_event_loop_ == loop) {
    return f(loop);
  }

  Owned<Promise<T>> promise(new Promise<T>());

  Future<T> future = promise->future();

  // Enqueue the function.
  {
    std::lock_guard<std::mutex> guard(functions_mutexes[loop_index.index]);
    functions[loop_index.index].push(
        lambda::bind(&_run_in_event_loop<T>, loop, f, promise));
  }

  // Interrupt the loop.
  ev_async_send(loop, &async_watchers[loop_index.index]);

  return future;
}

} // namespace process {

#endif // __LIBEV_HPP__
