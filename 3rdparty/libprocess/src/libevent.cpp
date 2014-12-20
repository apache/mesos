#include <unistd.h>

#include <event2/event.h>
#include <event2/thread.h>

#include <process/logging.hpp>

#include "event_loop.hpp"
#include "libevent.hpp"

namespace process {

struct event_base* base = NULL;

void* EventLoop::run(void*)
{
  int result = event_base_loop(base, 0);
  if (result < 0) {
    LOG(FATAL) << "Failed to run event loop";
  } else if (result == 1) {
    VLOG(1) << "Finished running event loop due to lack of events";
  }

  return NULL;
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
