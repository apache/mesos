#include <event2/event.h>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp> // For process::initialize.

#include "libevent.hpp"

namespace process {

namespace io {
namespace internal {

struct Poll
{
  Promise<short> promise;
  event* ev;
};


void pollCallback(evutil_socket_t, short what, void* arg)
{
  Poll* poll = reinterpret_cast<Poll*>(arg);

  if (poll->promise.future().hasDiscard()) {
    poll->promise.discard();
  } else {
    // Convert libevent specific EV_READ / EV_WRITE to io::* specific
    // values of these enumerations.
    short events =
      ((what & EV_READ) ? io::READ : 0) | ((what & EV_WRITE) ? io::WRITE : 0);

    poll->promise.set(events);
  }

  event_free(poll->ev);
  delete poll;
}


void pollDiscard(event* ev)
{
  event_active(ev, EV_READ, 0);
}

} // namespace internal {


Future<short> poll(int fd, short events)
{
  process::initialize();

  internal::Poll* poll = new internal::Poll();

  Future<short> future = poll->promise.future();

  // Convert io::READ / io::WRITE to libevent specific values of these
  // enumerations.
  short what =
    ((events & io::READ) ? EV_READ : 0) | ((events & io::WRITE) ? EV_WRITE : 0);

  poll->ev = event_new(base, fd, what, &internal::pollCallback, poll);
  if (poll->ev == NULL) {
    LOG(FATAL) << "Failed to poll, event_new";
  }

  event_add(poll->ev, NULL);

  return future
    .onDiscard(lambda::bind(&internal::pollDiscard, poll->ev));
}

} // namespace io {
} // namespace process {
