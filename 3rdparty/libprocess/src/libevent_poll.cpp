/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

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
