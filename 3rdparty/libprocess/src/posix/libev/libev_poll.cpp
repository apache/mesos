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

#include <memory>

#include <process/future.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/lambda.hpp>

#include "libev.hpp"

namespace process {

// Data necessary for polling so we can discard polling and actually
// stop it in the event loop.
struct Poll
{
  Poll()
  {
    // Need to explicitly instantiate the watchers.
    watcher.io.reset(new ev_io());
    watcher.async.reset(new ev_async());
  }

  // An I/O watcher for checking for readability or writeability and
  // an async watcher for being able to discard the polling.
  struct {
    std::shared_ptr<ev_io> io;
    std::shared_ptr<ev_async> async;
  } watcher;

  Promise<short> promise;
};


// Event loop callback when I/O is ready on polling file descriptor.
void polled(struct ev_loop* loop, ev_io* watcher, int revents)
{
  Poll* poll = (Poll*) watcher->data;

  ev_io_stop(loop, poll->watcher.io.get());

  // Stop the async watcher (also clears if pending so 'discard_poll'
  // will not get invoked and we can delete 'poll' here).
  ev_async_stop(loop, poll->watcher.async.get());

  poll->promise.set(revents);

  delete poll;
}


// Event loop callback when future associated with polling file
// descriptor has been discarded.
void discard_poll(struct ev_loop* loop, ev_async* watcher, int revents)
{
  Poll* poll = (Poll*) watcher->data;

  // Check and see if we have a pending 'polled' callback and if so
  // let it "win".
  if (ev_is_pending(poll->watcher.io.get())) {
    return;
  }

  ev_async_stop(loop, poll->watcher.async.get());

  // Stop the I/O watcher (but note we check if pending above) so it
  // won't get invoked and we can delete 'poll' here.
  ev_io_stop(loop, poll->watcher.io.get());

  poll->promise.discard();

  delete poll;
}


namespace io {
namespace internal {

// Helper/continuation of 'poll' on future discard.
void _poll(struct ev_loop* loop, const std::shared_ptr<ev_async>& async)
{
  ev_async_send(loop, async.get());
}


Future<short> poll(struct ev_loop* loop, int_fd fd, short events)
{
  Poll* poll = new Poll();

  // Have the watchers data point back to the struct.
  poll->watcher.async->data = poll;
  poll->watcher.io->data = poll;

  // Get a copy of the future to avoid any races with the event loop.
  Future<short> future = poll->promise.future();

  // Initialize and start the async watcher.
  ev_async_init(poll->watcher.async.get(), discard_poll);
  ev_async_start(loop, poll->watcher.async.get());

  // Make sure we stop polling if a discard occurs on our future.
  // Note that it's possible that we'll invoke '_poll' when someone
  // does a discard even after the polling has already completed, but
  // in this case while we will interrupt the event loop since the
  // async watcher has already been stopped we won't cause
  // 'discard_poll' to get invoked.
  future.onDiscard(lambda::bind(&_poll, loop, poll->watcher.async));

  // Initialize and start the I/O watcher.
  ev_io_init(poll->watcher.io.get(), polled, fd, events);
  ev_io_start(loop, poll->watcher.io.get());

  return future;
}

} // namespace internal {


Future<short> poll(int_fd fd, short events)
{
  process::initialize();

  // TODO(benh): Check if the file descriptor is non-blocking?
  return run_in_event_loop<short>(
      get_loop(fd),
      lambda::bind(&internal::poll, lambda::_1, fd, events));
}

} // namespace io {
} // namespace process {
