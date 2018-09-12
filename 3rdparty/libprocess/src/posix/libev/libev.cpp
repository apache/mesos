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

#include <ev.h>
#include <signal.h>

#include <mutex>
#include <queue>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>

#include "event_loop.hpp"
#include "libev.hpp"

namespace process {

ev_async async_watcher;
// We need an asynchronous watcher to receive the request to shutdown.
ev_async shutdown_watcher;

// Define the initial values for all of the declarations made in
// libev.hpp (since these need to live in the static data space).
struct ev_loop* loop = nullptr;

std::queue<ev_io*>* watchers = new std::queue<ev_io*>();

std::mutex* watchers_mutex = new std::mutex();

std::queue<lambda::function<void()>>* functions =
  new std::queue<lambda::function<void()>>();

thread_local bool* _in_event_loop_ = nullptr;


void handle_async(struct ev_loop* loop, ev_async* _, int revents)
{
  std::queue<lambda::function<void()>> run_functions;
  synchronized (watchers_mutex) {
    // Start all the new I/O watchers.
    while (!watchers->empty()) {
      ev_io* watcher = watchers->front();
      watchers->pop();
      ev_io_start(loop, watcher);
    }

    // Swap the functions into a temporary queue so that we can invoke
    // them outside of the mutex.
    std::swap(run_functions, *functions);
  }

  // Running the functions outside of the mutex reduces locking
  // contention as these are arbitrary functions that can take a long
  // time to execute. Doing this also avoids a deadlock scenario where
  // (A) mutexes are acquired before calling `run_in_event_loop`,
  // followed by locking (B) `watchers_mutex`. If we executed the
  // functions inside the mutex, then the locking order violation
  // would be this function acquiring the (B) `watchers_mutex`
  // followed by the arbitrary function acquiring the (A) mutexes.
  while (!run_functions.empty()) {
    (run_functions.front())();
    run_functions.pop();
  }
}


void handle_shutdown(struct ev_loop* loop, ev_async* _, int revents)
{
  ev_unloop(loop, EVUNLOOP_ALL);
}


void EventLoop::initialize()
{
  // libev, when built with child process watcher support (the
  // EV_CHILD_ENABLE feature flag), will install a SIGCHLD handler
  // and wait on all processes. We need to save and restore the
  // current signal handler in order to disable this behavior.
  struct sigaction chldHandler;

  PCHECK(::sigaction(SIGCHLD, nullptr, &chldHandler) == 0);

  loop = ev_default_loop(EVFLAG_AUTO);

  PCHECK(::sigaction(SIGCHLD, &chldHandler, nullptr) == 0);

  ev_async_init(&async_watcher, handle_async);
  ev_async_init(&shutdown_watcher, handle_shutdown);

  ev_async_start(loop, &async_watcher);
  ev_async_start(loop, &shutdown_watcher);
}


namespace internal {

void handle_delay(struct ev_loop* loop, ev_timer* timer, int revents)
{
  lambda::function<void()>* function =
    reinterpret_cast<lambda::function<void()>*>(timer->data);
  (*function)();
  delete function;
  ev_timer_stop(loop, timer);
  delete timer;
}


Future<Nothing> delay(
    const Duration& duration,
    const lambda::function<void()>& function)
{
  ev_timer* timer = new ev_timer();
  timer->data = reinterpret_cast<void*>(new lambda::function<void()>(function));

  // Determine the 'after' parameter to pass to libev and set it to 0
  // in the event that it's negative so that we always make sure to
  // invoke 'function' even if libev doesn't support negative 'after'
  // values.
  double after = duration.secs();

  if (after < 0) {
    after = 0;
  }

  const double repeat = 0.0;

  ev_timer_init(timer, handle_delay, after, repeat);
  ev_timer_start(loop, timer);

  return Nothing();
}

} // namespace internal {


void EventLoop::delay(
    const Duration& duration,
    const lambda::function<void()>& function)
{
  run_in_event_loop<Nothing>(
      lambda::bind(&internal::delay, duration, function));
}


double EventLoop::time()
{
  // TODO(benh): Versus ev_now()?
  return ev_time();
}


void EventLoop::run()
{
  __in_event_loop__ = true;

  ev_loop(loop, 0);

  __in_event_loop__ = false;
}


void EventLoop::stop()
{
  ev_async_send(loop, &shutdown_watcher);
}

} // namespace process {
