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

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/read.hpp>
#include <stout/os/socket.hpp>
#include <stout/os/write.hpp>

#include "io_internal.hpp"

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
} // namespace io {
} // namespace process {
