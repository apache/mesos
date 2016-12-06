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

#include <event2/event.h>

#include <memory>

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
  std::shared_ptr<event> ev;
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

  // Deleting the `poll` also destructs `ev` and hence triggers `event_free`,
  // which makes the event non-pending.
  delete poll;
}


void pollDiscard(const std::weak_ptr<event>& ev, short events)
{
  // Discarding inside the event loop prevents `pollCallback()` from being
  // called twice if the future is discarded.
  run_in_event_loop([=]() {
    std::shared_ptr<event> shared = ev.lock();
    // If `ev` cannot be locked `pollCallback` already ran. If it was locked
    // but not pending, `pollCallback` is scheduled to be executed.
    if (static_cast<bool>(shared) &&
        event_pending(shared.get(), events, nullptr)) {
      // `event_active` will trigger the `pollCallback` to be executed.
      event_active(shared.get(), EV_READ, 0);
    }
  });
}

} // namespace internal {


Future<short> poll(int_fd fd, short events)
{
  process::initialize();

  internal::Poll* poll = new internal::Poll();

  Future<short> future = poll->promise.future();

  // Convert io::READ / io::WRITE to libevent specific values of these
  // enumerations.
  short what =
    ((events & io::READ) ? EV_READ : 0) | ((events & io::WRITE) ? EV_WRITE : 0);

  // Bind `event_free` to the destructor of the `ev` shared pointer
  // guaranteeing that the event will be freed only once.
  poll->ev.reset(
      event_new(base, fd, what, &internal::pollCallback, poll),
      event_free);

  if (poll->ev == nullptr) {
    LOG(FATAL) << "Failed to poll, event_new";
  }

  // Using a `weak_ptr` prevents `ev` to become a dangling pointer if
  // the returned future is discarded after the event is triggered.
  // The `weak_ptr` needs to be created before `event_add` in case
  // the event is ready and the callback is executed before creating
  // `ev`.
  std::weak_ptr<event> ev(poll->ev);

  event_add(poll->ev.get(), nullptr);

  return future
    .onDiscard(lambda::bind(&internal::pollDiscard, ev, what));
}

} // namespace io {
} // namespace process {
