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

#include <condition_variable>
#include <mutex>
#include <queue>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>

#include "event_loop.hpp"
#include "libev.hpp"

namespace process {

namespace internal {

// We need a latch (available in C++20 but not in C++11) to
// wait for the loop to finish, so define a simple one here.
// To keep this simple, this only allows 1 triggering thread
// and 1 waiting thread.
//
// TODO(bmahler): Replace this with std::latch in C++20.
class Latch
{
public:
  void trigger()
  {
    std::unique_lock<std::mutex> lock(mutex);
    triggered = true;
    condition.notify_all();
  }

  void wait()
  {
    std::unique_lock<std::mutex> lock(mutex);
    while (!triggered) {
      condition.wait(lock);
    }
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  bool triggered = false;
};

} // namespace internal {

ev_async async_watcher;
// We need an asynchronous watcher to receive the request to shutdown.
ev_async shutdown_watcher;

// Define the initial values for all of the declarations made in
// libev.hpp (since these need to live in the static data space).
struct ev_loop* loop = nullptr;

internal::Latch* loop_destroy_latch = nullptr;

std::mutex* functions_mutex = new std::mutex();

std::queue<lambda::function<void()>>* functions =
  new std::queue<lambda::function<void()>>();

thread_local bool* _in_event_loop_ = nullptr;


void handle_async(struct ev_loop* loop, ev_async* _, int revents)
{
  std::queue<lambda::function<void()>> run_functions;
  synchronized (functions_mutex) {
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
  loop = CHECK_NOTNULL(ev_loop_new(EVFLAG_AUTO));

  ev_async_init(&async_watcher, handle_async);
  ev_async_init(&shutdown_watcher, handle_shutdown);

  ev_async_start(loop, &async_watcher);
  ev_async_start(loop, &shutdown_watcher);

  loop_destroy_latch = new internal::Latch();
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

  loop_destroy_latch->trigger();
}


void EventLoop::stop()
{
  ev_async_send(loop, &shutdown_watcher);

  loop_destroy_latch->wait();

  delete loop_destroy_latch;
  loop_destroy_latch = nullptr;

  ev_loop_destroy(loop);
  loop = nullptr;
}

} // namespace process {
