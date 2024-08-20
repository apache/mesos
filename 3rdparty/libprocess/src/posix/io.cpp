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

#ifdef __linux__
#include <memory>

#include <sys/inotify.h>
#endif

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/read.hpp>
#include <stout/os/socket.hpp>
#include <stout/os/write.hpp>

#include "io_internal.hpp"

using std::default_delete;
using std::shared_ptr;
using std::string;
using std::weak_ptr;

namespace process {
namespace io {
namespace internal {

Future<size_t> read(int_fd fd, void* data, size_t size)
{
  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  return loop(
      None(),
      [=]() -> Future<Option<size_t>> {
        // Because the file descriptor is non-blocking, we call
        // read()/recv() immediately. If no data is available than
        // we'll call `poll` and block. We also observed that for some
        // combination of libev and Linux kernel versions, the poll
        // would block for non-deterministically long periods of
        // time. This may be fixed in a newer version of libev (we use
        // 3.8 at the time of writing this comment).
        ssize_t length = os::read(fd, data, size);
        if (length < 0) {
#ifdef __WINDOWS__
          WindowsSocketError error;
#else
          ErrnoError error;
#endif // __WINDOWS__

          if (!net::is_restartable_error(error.code) &&
              !net::is_retryable_error(error.code)) {
            return Failure(error.message);
          }

          return None();
        }

        return length;
      },
      [=](const Option<size_t>& length) -> Future<ControlFlow<size_t>> {
        // Restart/retry if we don't yet have a result.
        if (length.isNone()) {
          return io::poll(fd, io::READ)
            .then([](short event) -> ControlFlow<size_t> {
              CHECK_EQ(io::READ, event);
              return Continue();
            });
        }
        return Break(length.get());
      });
}


Future<size_t> write(int_fd fd, const void* data, size_t size)
{
  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  return loop(
      None(),
      [=]() -> Future<Option<size_t>> {
        ssize_t length = os::write(fd, data, size);

        if (length < 0) {
#ifdef __WINDOWS__
          WindowsSocketError error;
#else
          ErrnoError error;
#endif // __WINDOWS__

          if (!net::is_restartable_error(error.code) &&
              !net::is_retryable_error(error.code)) {
            return Failure(error.message);
          }

          return None();
        }

        return length;
      },
      [=](const Option<size_t>& length) -> Future<ControlFlow<size_t>> {
        // Restart/retry if we don't yet have a result.
        if (length.isNone()) {
          return io::poll(fd, io::WRITE)
            .then([](short event) -> ControlFlow<size_t> {
              CHECK_EQ(io::WRITE, event);
              return Continue();
            });
        }
        return Break(length.get());
      });
}


Try<Nothing> prepare_async(int_fd fd)
{
  return os::nonblock(fd);
}


Try<bool> is_async(int_fd fd)
{
  return os::isNonblock(fd);
}

} // namespace internal {

#ifdef __linux__

Watcher::Watcher(int inotify_fd) : inotify_fd(inotify_fd), data(new Data()) {}


Watcher::Data::~Data()
{
  // When the last reference of data goes away, we discard the read loop
  // which should ensure that the loop future transitions, at which point
  // the inotify fd will get closed.
  read_loop.discard();
}


void Watcher::run()
{
  // For now, we only use a small buffer that is sufficient for reading
  // *at least* 32 events, but the caller may want to customize the buffer
  // size depending on the expected volume of events.
  size_t buffer_size = 32 * (sizeof(inotify_event) + NAME_MAX + 1);
  shared_ptr<char> buffer(new char[buffer_size], default_delete<char[]>());

  // We take a weak pointer here to avoid keeping Data alive forever, when
  // the caller throws away the last Watcher copy, we want Data to be destroyed
  // and the loop to be stopped.
  weak_ptr<Data> weak_data = data;
  int fd = inotify_fd;
  data->read_loop = loop(
      [fd, buffer, buffer_size]() {
        return io::read(fd, buffer.get(), buffer_size);
      },
      [weak_data, buffer](size_t read) -> Future<ControlFlow<Nothing>> {
        if (read == 0) {
          return Failure("Unexpected EOF");
        }

        // If we can't get the shared pointer, Data is destroyed and we
        // need to stop the loop.
        shared_ptr<Data> data = weak_data.lock();
        if (!data) {
          return Break();
        }

        size_t offset = 0;
        for (offset = 0; offset <= read - sizeof(inotify_event);) {
          inotify_event* event = (inotify_event*) &(buffer.get()[offset]);
          offset += sizeof(inotify_event) + event->len;

          if (event-> mask & IN_Q_OVERFLOW) {
            return Failure("inotify event overflow");
          }

          // For IN_IGNORED generated by inotify_rm_watch, we've already
          // remove the path from our tracking maps. For other cases of
          // IN_IGNORED (e.g. IN_DELETE_SELF, IN_UNMOUNT, etc), we remove the
          // path during those events rather than the subsequent IN_IGNORED.
          if (event->mask & IN_IGNORED) {
            continue;
          }

          Event e;

          if (event->mask & IN_MODIFY) {
            e.type = Event::Write;
          }
          if (event->mask & IN_MOVE_SELF) {
            e.type = Event::Rename;
          }
          if (event->mask & IN_DELETE_SELF || event->mask & IN_UNMOUNT) {
            e.type = Event::Remove;
          }

          synchronized (data->lock) {
            Option<string> path = data->wd_to_path.get(event->wd);

            if (path.isNone()) {
              continue; // Unknown watch, likely we just removed this watch.
            }

            e.path = std::move(*path);

            if (event->mask & IN_MOVE_SELF
                || event->mask & IN_DELETE_SELF
                || event->mask & IN_UNMOUNT) {
              data->wd_to_path.erase(event->wd);
              data->path_to_wd.erase(*path);
            }

            data->events.put(std::move(e));
          }
        }

        if (offset != read) {
          return Failure("Unexpected partial read from inotify");
        }

        return Continue();
      });

  data->read_loop
    .onFailed([weak_data](const string& message) {
      shared_ptr<Data> data = weak_data.lock();
      if (data) {
        Watcher::Event e;
        e.type = Watcher::Event::Failure;
        e.path = message;
        data->events.put(e);
      }
    });

  // We need to close the inotify fd whenever the loop stops.
  data->read_loop
    .onAny([fd]() {
      ::close(fd);
    });
}


Try<Nothing> Watcher::add(const string& path)
{
  // Since we only currently support watching a file (not directories),
  // and we're only interested in modifications to the file contents,
  // we only need to watch for the following relevant events:
  int mask = IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF;

#ifdef IN_MASK_CREATE
  // Fail with EEXIST if a watch already exists. This ensures that new
  // watches don't modify existing ones, either because the path gets
  // watched multiple times, or when two paths refer to the same inode.
  mask = mask | IN_MASK_CREATE;
#endif


  if (os::stat::isdir(path)) {
    return Error("Directories are not supported");
  }

  synchronized (data->lock) {
    if (data->path_to_wd.get(path).isSome()) {
      return Error("Path is already added");
    }

    int wd = inotify_add_watch(inotify_fd, path.c_str(), mask);
    if (wd < 0) {
      return ErrnoError("Failed to inotify_add_watch");
    }

    data->wd_to_path[wd] = path;
    data->path_to_wd[path] = wd;
  }

  return Nothing();
}


Try<Nothing> Watcher::remove(const string& path)
{
  Option<int> wd;
  synchronized (data->lock) {
    wd = data->path_to_wd.get(path);
    if (wd.isNone()) {
      // Note that the path may have been implicitly removed via the
      // read loop when the file gets removed. Should we treat this as
      // a failure? Or a no-op?
      return Nothing();
    }

    data->wd_to_path.erase(*wd);
    data->path_to_wd.erase(path);
  }

  // Note that removing the watch will trigger an IN_IGNORED event.
  if (inotify_rm_watch(inotify_fd, *wd) < 0) {
    return ErrnoError("Failed to inotify_rm_watch");
  }

  return Nothing();
}


Queue<Watcher::Event> Watcher::events()
{
  return data->events;
}


Try<Watcher> create_watcher()
{
  int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd < 0) {
    return ErrnoError("Failed to inotify_init1");
  }

  Watcher watcher(inotify_fd);
  watcher.run();
  return watcher;
}

#endif // __linux__

} // namespace io {
} // namespace process {
