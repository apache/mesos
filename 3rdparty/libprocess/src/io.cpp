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

#include <memory>
#include <string>

#include <boost/shared_array.hpp>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/read.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os/write.hpp>

using std::string;
using std::vector;

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

} // namespace internal {


Future<size_t> read(int_fd fd, void* data, size_t size)
{
  process::initialize();

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    return Failure("Failed to check if file descriptor was non-blocking: " +
                   nonblock.error());
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    return Failure("Expected a non-blocking file descriptor");
  }

  return internal::read(fd, data, size);
}


Future<size_t> write(int_fd fd, const void* data, size_t size)
{
  process::initialize();

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    return Failure(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    return Failure("Expected a non-blocking file descriptor");
  }

  return internal::write(fd, data, size);
}


namespace internal {

Future<Nothing> splice(
    int_fd from,
    int_fd to,
    size_t chunk,
    const vector<lambda::function<void(const string&)>>& hooks)
{
  boost::shared_array<char> data(new char[chunk]);
  return loop(
      None(),
      [=]() {
        return io::read(from, data.get(), chunk);
      },
      [=](size_t length) -> Future<ControlFlow<Nothing>> {
        if (length == 0) { // EOF.
          return Break();
        }

        // Send the data to the redirect hooks.
        const string s = string(data.get(), length);
        foreach (const lambda::function<void(const string&)>& hook, hooks) {
          hook(s);
        }

        return io::write(to, s)
          .then([]() -> Future<ControlFlow<Nothing>> {
            return Continue();
          });
      });
}


} // namespace internal {


Future<string> read(int_fd fd)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone accidentally
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(os::strerror(EBADF));
  }

  Try<int_fd> dup = os::dup(fd);
  if (dup.isError()) {
    return Failure(dup.error());
  }

  fd = dup.get();

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  // TODO(benh): Wrap up this data as a struct, use 'Owner'.
  // TODO(bmahler): For efficiency, use a rope for the buffer.
  std::shared_ptr<string> buffer(new string());
  boost::shared_array<char> data(new char[BUFFERED_READ_SIZE]);

  return loop(
      None(),
      [=]() {
        return io::read(fd, data.get(), BUFFERED_READ_SIZE);
      },
      [=](size_t length) -> ControlFlow<string> {
        if (length == 0) { // EOF.
          return Break(std::move(*buffer));
        }
        buffer->append(data.get(), length);
        return Continue();
      })
    .onAny([fd]() {
      os::close(fd);
    });
}


Future<Nothing> write(int_fd fd, const string& data)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone accidentally
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(os::strerror(EBADF));
  }

  Try<int_fd> dup = os::dup(fd);
  if (dup.isError()) {
    return Failure(dup.error());
  }

  fd = dup.get();

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(fd);
  if (cloexec.isError()) {
    os::close(fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor non-blocking.
  Try<Nothing> nonblock = os::nonblock(fd);
  if (nonblock.isError()) {
    os::close(fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }

  // We store `data.size()` so that we can just use `size` in the
  // second lambda below versus having to make a copy of `data` in
  // both lambdas since `data` might be very big and two copies could
  // be expensive!
  const size_t size = data.size();

  // We need to share the `index` between both lambdas below.
  std::shared_ptr<size_t> index(new size_t(0));

  return loop(
      None(),
      [=]() {
        return io::write(fd, data.data() + *index, size - *index);
      },
      [=](size_t length) -> ControlFlow<Nothing> {
        if ((*index += length) != size) {
          return Continue();
        }
        return Break();
      })
    .onAny([fd]() {
        os::close(fd);
    });
}


Future<Nothing> redirect(
    int_fd from,
    Option<int_fd> to,
    size_t chunk,
    const vector<lambda::function<void(const string&)>>& hooks)
{
  // Make sure we've got "valid" file descriptors.
  if (from < 0 || (to.isSome() && to.get() < 0)) {
    return Failure(os::strerror(EBADF));
  }

  if (to.isNone()) {
    // Open up /dev/null that we can splice into.
    Try<int_fd> open = os::open(os::DEV_NULL, O_WRONLY | O_CLOEXEC);

    if (open.isError()) {
      return Failure("Failed to open /dev/null for writing: " + open.error());
    }

    to = open.get();
  } else {
    // Duplicate 'to' so that we're in control of its lifetime.
    Try<int_fd> dup = os::dup(to.get());
    if (dup.isError()) {
      return Failure(dup.error());
    }

    to = dup.get();
  }

  CHECK_SOME(to);

  // Duplicate 'from' so that we're in control of its lifetime.
  Try<int_fd> dup = os::dup(from);
  if (dup.isError()) {
    os::close(to.get());
    return Failure(ErrnoError("Failed to duplicate 'from' file descriptor"));
  }

  from = dup.get();

  // Set the close-on-exec flag (no-op if already set).
  Try<Nothing> cloexec = os::cloexec(from);
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'from': " + cloexec.error());
  }

  cloexec = os::cloexec(to.get());
  if (cloexec.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to set close-on-exec on 'to': " + cloexec.error());
  }

  // Make the file descriptors non-blocking (no-op if already set).
  Try<Nothing> nonblock = os::nonblock(from);
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'from' non-blocking: " + nonblock.error());
  }

  nonblock = os::nonblock(to.get());
  if (nonblock.isError()) {
    os::close(from);
    os::close(to.get());
    return Failure("Failed to make 'to' non-blocking: " + nonblock.error());
  }

  // NOTE: We wrap `os::close` in a lambda to disambiguate on Windows.
  return internal::splice(from, to.get(), chunk, hooks)
    .onAny([from]() { os::close(from); })
    .onAny([to]() { os::close(to.get()); });
}

} // namespace io {
} // namespace process {
