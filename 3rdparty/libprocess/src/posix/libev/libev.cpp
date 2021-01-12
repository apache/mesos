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

#include <glog/logging.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>

#include <stout/os/getenv.hpp>

#include "event_loop.hpp"
#include "libev.hpp"

using std::string;
using std::thread;
using std::vector;

namespace process {

namespace internal {

// We need a latch (available in C++20 but not in C++11) to
// wait for all loops to finish, so define a simple one here.
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


size_t num_loops = 1L;

// Array of async watchers to receive the request to shutdown
// each event loop.
ev_async* shutdown_watchers = nullptr;

ev_async* async_watchers = nullptr;

struct ev_loop** loops = nullptr;

internal::Latch* loop_destroy_latch = nullptr;

std::mutex* functions_mutexes = nullptr;
std::queue<lambda::function<void()>>* functions = nullptr;

thread_local struct ev_loop* _in_event_loop_ = nullptr;


void handle_async(struct ev_loop* loop, ev_async* async, int revents)
{
  // This is a hack to use the data pointer as integer storage.
  size_t loop_index = reinterpret_cast<size_t>(async->data);

  std::queue<lambda::function<void()>> run_functions;

  // Swap the functions into a temporary queue so that we can invoke
  // them outside of the mutex.
  {
    std::lock_guard<std::mutex> guard(functions_mutexes[loop_index]);
    std::swap(run_functions, functions[loop_index]);
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


LoopIndex get_loop(int_fd fd)
{
  return LoopIndex(static_cast<size_t>(fd) % num_loops);
}


void EventLoop::initialize()
{
  // TODO(bmahler): Since this is a new feature, we stick to the
  // old behavior of a single event loop thread. But, once this
  // code is known to be stable and helpful, we can consider
  // increasing the default.
  num_loops = 1;

  // TODO(bmahler): Load this via a Flag object to eliminate this boilerplate.
  constexpr char env_var[] = "LIBPROCESS_LIBEV_NUM_IO_THREADS";
  Option<string> value = os::getenv(env_var);
  if (value.isSome()) {
    constexpr size_t maxval = 1024;
    Try<size_t> number = numify<size_t>(value->c_str());
    if (number.isSome() && number.get() > 0 && number.get() <= maxval) {
      VLOG(1) << "Overriding default number of libev io threads"
              << " (" << num_loops << "), using the value "
              << env_var << "=" << *number << " instead";
      num_loops = number.get();
    } else {
      EXIT(EXIT_FAILURE)
          << "Invalid value '" << value.get() << "' for " << env_var
          << "; Valid values are integers in the range 1 to " << maxval;
    }
  }

  loops = new struct ev_loop*[num_loops];

  for (size_t i = 0; i < num_loops; ++i) {
    loops[i] = CHECK_NOTNULL(ev_loop_new(EVFLAG_AUTO));
  }

  functions_mutexes = new std::mutex[num_loops];
  functions = new std::queue<lambda::function<void()>>[num_loops];

  async_watchers = new struct ev_async[num_loops];
  shutdown_watchers = new struct ev_async[num_loops];

  for (size_t i = 0; i < num_loops; ++i) {
    // We need to pass the loop index as data, so instead of putting
    // it on the heap we just use the pointer as integer storage.
    async_watchers[i].data = reinterpret_cast<void*>(i);

    ev_async_init(&async_watchers[i], handle_async);
    ev_async_init(&shutdown_watchers[i], handle_shutdown);

    ev_async_start(loops[i], &async_watchers[i]);
    ev_async_start(loops[i], &shutdown_watchers[i]);
  }

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
    struct ev_loop* loop,
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
  // TODO(bmahler): The current approach is to send all timers to the
  // loop 0, but we could round robin them.
  run_in_event_loop<Nothing>(
      get_loop(0),
      lambda::bind(&internal::delay, lambda::_1, duration, function));
}


double EventLoop::time()
{
  // TODO(benh): Versus ev_now()?
  return ev_time();
}


void EventLoop::run()
{
  // Loop 0 runs on this thread, the others get their own threads.
  vector<thread> threads;
  threads.reserve(num_loops - 1);

  for (size_t i = 1; i < num_loops; ++i) {
    threads.push_back(thread([i]() {
      _in_event_loop_ = loops[i];
      ev_loop(loops[i], 0);
      _in_event_loop_ = nullptr;
    }));
  }

  _in_event_loop_ = loops[0];
  ev_loop(loops[0], 0);
  _in_event_loop_ = nullptr;

  foreach (thread& t, threads) {
    t.join();
  }

  loop_destroy_latch->trigger();
}


void EventLoop::stop()
{
  // Stop the loops and wait for them to finish before destroying them.
  for (size_t i = 0; i < num_loops; ++i) {
    ev_async_send(loops[i], &shutdown_watchers[i]);
  }

  loop_destroy_latch->wait();

  delete loop_destroy_latch;
  loop_destroy_latch = nullptr;

  for (size_t i = 0; i < num_loops; ++i) {
    ev_loop_destroy(loops[i]);
  }

  delete[] loops;
  loops = nullptr;

  delete[] functions;
  functions = nullptr;

  delete[] functions_mutexes;
  functions_mutexes = nullptr;

  delete[] async_watchers;
  async_watchers = nullptr;

  delete[] shutdown_watchers;
  shutdown_watchers = nullptr;
}

} // namespace process {
