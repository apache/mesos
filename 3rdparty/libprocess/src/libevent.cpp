#include <unistd.h>

#include <event2/event.h>
#include <event2/thread.h>

#include <process/logging.hpp>

#include <stout/synchronized.hpp>

#include "event_loop.hpp"
#include "libevent.hpp"

namespace process {

struct event_base* base = NULL;


void* EventLoop::run(void*)
{
  do {
    int result = event_base_loop(base, EVLOOP_ONCE);
    if (result < 0) {
      LOG(FATAL) << "Failed to run event loop";
    } else if (result == 1) {
      VLOG(1) << "All events handled, continuing event loop";
      continue;
    } else if (event_base_got_break(base)) {
      break;
    } else if (event_base_got_exit(base)) {
      break;
    }
  } while (true);
  return NULL;
}


namespace internal {

struct Delay
{
  lambda::function<void(void)> function;
  event* timer;
};

void handle_delay(int, short, void* arg)
{
  Delay* delay = reinterpret_cast<Delay*>(arg);
  delay->function();
  event_free(delay->timer);
  delete delay;
}

}  // namespace internal {


void EventLoop::delay(
    const Duration& duration,
    const lambda::function<void(void)>& function)
{
  internal::Delay* delay = new internal::Delay();
  delay->timer = evtimer_new(base, &internal::handle_delay, delay);
  if (delay->timer == NULL) {
    LOG(FATAL) << "Failed to delay, evtimer_new";
  }

  delay->function = function;

  timeval t{0, 0};
  if (duration > Seconds(0)) {
    t = duration.timeval();
  }

  evtimer_add(delay->timer, &t);
}


double EventLoop::time()
{
  // Get the cached time if running the event loop, or call
  // gettimeofday() to get the current time. Since a lot of logic in
  // libprocess depends on time math, we want to log fatal rather than
  // cause logic errors if the time fails.
  timeval t;
  if (event_base_gettimeofday_cached(base, &t) < 0) {
    LOG(FATAL) << "Failed to get time, event_base_gettimeofday_cached";
  }

  return Duration(t).secs();
}


void EventLoop::initialize()
{
  if (evthread_use_pthreads() < 0) {
    LOG(FATAL) << "Failed to initialize, evthread_use_pthreads";
  }

  // This enables debugging of libevent calls. We can remove this
  // when the implementation settles and after we gain confidence.
  event_enable_debug_mode();

  base = event_base_new();
  if (base == NULL) {
    LOG(FATAL) << "Failed to initialize, event_base_new";
  }
}

} // namespace process {
