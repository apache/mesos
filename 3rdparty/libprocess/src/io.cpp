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
#include <process/process.hpp> // For process::initialize.

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/os/strerror.hpp>
#include <stout/try.hpp>

using std::string;

namespace process {
namespace io {
namespace internal {

enum ReadFlags
{
  NONE = 0,
  PEEK
};


void read(
    int fd,
    void* data,
    size_t size,
    ReadFlags flags,
    const std::shared_ptr<Promise<size_t>>& promise,
    const Future<short>& future)
{
  // Ignore this function if the read operation has been discarded.
  if (promise->future().hasDiscard()) {
    CHECK(!future.isPending());
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    ssize_t length;
    if (flags == NONE) {
      length = ::read(fd, data, size);
    } else { // PEEK.
      // In case 'fd' is not a socket ::recv() will fail with ENOTSOCK and the
      // error will be propagted out.
      length = ::recv(fd, data, size, MSG_PEEK);
    }

    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the read operation.
        Future<short> future =
          io::poll(fd, process::io::READ).onAny(
              lambda::bind(&internal::read,
                           fd,
                           data,
                           size,
                           flags,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(os::strerror(errno));
      }
    } else {
      promise->set(length);
    }
  }
}


void write(
    int fd,
    const void* data,
    size_t size,
    const std::shared_ptr<Promise<size_t>>& promise,
    const Future<short>& future)
{
  // Ignore this function if the write operation has been discarded.
  if (promise->future().hasDiscard()) {
    promise->discard();
    return;
  }

  if (size == 0) {
    promise->set(0);
    return;
  }

  if (future.isDiscarded()) {
    promise->fail("Failed to poll: discarded future");
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    ssize_t length = ::write(fd, data, size);

    if (length < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        // Restart the write operation.
        Future<short> future =
          io::poll(fd, process::io::WRITE).onAny(
              lambda::bind(&internal::write,
                           fd,
                           data,
                           size,
                           promise,
                           lambda::_1));

        // Stop polling if a discard occurs on our future.
        promise->future().onDiscard(
            lambda::bind(&process::internal::discard<short>,
                         WeakFuture<short>(future)));
      } else {
        // Error occurred.
        promise->fail(os::strerror(errno));
      }
    } else {
      // TODO(benh): Retry if 'length' is 0?
      promise->set(length);
    }
  }
}

} // namespace internal {


Future<size_t> read(int fd, void* data, size_t size)
{
  process::initialize();

  std::shared_ptr<Promise<size_t>> promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call read()
  // immediately. The read may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::read(fd, data, size, internal::NONE, promise, io::READ);

  return promise->future();
}


Future<size_t> write(int fd, const void* data, size_t size)
{
  process::initialize();

  std::shared_ptr<Promise<size_t>> promise(new Promise<size_t>());

  // Check the file descriptor.
  Try<bool> nonblock = os::isNonblock(fd);
  if (nonblock.isError()) {
    // The file descriptor is not valid (e.g., has been closed).
    promise->fail(
        "Failed to check if file descriptor was non-blocking: " +
        nonblock.error());
    return promise->future();
  } else if (!nonblock.get()) {
    // The file descriptor is not non-blocking.
    promise->fail("Expected a non-blocking file descriptor");
    return promise->future();
  }

  // Because the file descriptor is non-blocking, we call write()
  // immediately. The write may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::write(fd, data, size, promise, io::WRITE);

  return promise->future();
}


Future<size_t> peek(int fd, void* data, size_t size, size_t limit)
{
  process::initialize();

  // Make sure that the buffer is large enough.
  if (size < limit) {
    return Failure("Expected a large enough data buffer");
  }

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(os::strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

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

  std::shared_ptr<Promise<size_t>> promise(new Promise<size_t>());

  // Because the file descriptor is non-blocking, we call read()
  // immediately. The read may in turn call poll if necessary,
  // avoiding unnecessary polling. We also observed that for some
  // combination of libev and Linux kernel versions, the poll would
  // block for non-deterministically long periods of time. This may be
  // fixed in a newer version of libev (we use 3.8 at the time of
  // writing this comment).
  internal::read(fd, data, limit, internal::PEEK, promise, io::READ);
  promise->future().onAny(lambda::bind(&os::close, fd));

  return promise->future();
}


namespace internal {

Future<string> _read(
    int fd,
    const std::shared_ptr<string>& buffer,
    const boost::shared_array<char>& data,
    size_t length)
{
  return io::read(fd, data.get(), length)
    .then([=](size_t size) -> Future<string> {
      if (size == 0) { // EOF.
        return string(*buffer);
      }
      buffer->append(data.get(), size);
      return _read(fd, buffer, data, length);
    });
}


Future<Nothing> _write(
    int fd,
    Owned<string> data,
    size_t index)
{
  return io::write(fd, data->data() + index, data->size() - index)
    .then([=](size_t length) -> Future<Nothing> {
      if (index + length == data->size()) {
        return Nothing();
      }
      return _write(fd, data, index + length);
    });
}


void _splice(
    int from,
    int to,
    size_t chunk,
    boost::shared_array<char> data,
    std::shared_ptr<Promise<Nothing>> promise)
{
  // Stop splicing if a discard occured on our future.
  if (promise->future().hasDiscard()) {
    // TODO(benh): Consider returning the number of bytes already
    // spliced on discarded, or a failure. Same for the 'onDiscarded'
    // callbacks below.
    promise->discard();
    return;
  }

  // Note that only one of io::read or io::write is outstanding at any
  // one point in time thus the reuse of 'data' for both operations.

  Future<size_t> read = io::read(from, data.get(), chunk);

  // Stop reading (or potentially indefinitely polling) if a discard
  // occcurs on our future.
  promise->future().onDiscard(
      lambda::bind(&process::internal::discard<size_t>,
                   WeakFuture<size_t>(read)));

  read
    .onReady([=](size_t size) {
      if (size == 0) { // EOF.
        promise->set(Nothing());
      } else {
        // Note that we always try and complete the write, even if a
        // discard has occured on our future, in order to provide
        // semantics where everything read is written. The promise
        // will eventually be discarded in the next read.
        io::write(to, string(data.get(), size))
          .onReady([=]() { _splice(from, to, chunk, data, promise); })
          .onFailed([=](const string& message) { promise->fail(message); })
          .onDiscarded([=]() { promise->discard(); });
      }
    })
    .onFailed([=](const string& message) { promise->fail(message); })
    .onDiscarded([=]() { promise->discard(); });
}


Future<Nothing> splice(int from, int to, size_t chunk)
{
  boost::shared_array<char> data(new char[chunk]);

  // Rather than having internal::_splice return a future and
  // implementing internal::_splice as a chain of io::read and
  // io::write calls, we use an explicit promise that we pass around
  // so that we don't increase memory usage the longer that we splice.
  std::shared_ptr<Promise<Nothing>> promise(new Promise<Nothing>());

  Future<Nothing> future = promise->future();

  _splice(from, to, chunk, data, promise);

  return future;
}

} // namespace internal {


Future<string> read(int fd)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(os::strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

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

  return internal::_read(fd, buffer, data, BUFFERED_READ_SIZE)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> write(int fd, const std::string& data)
{
  process::initialize();

  // Get our own copy of the file descriptor so that we're in control
  // of the lifetime and don't crash if/when someone by accidently
  // closes the file descriptor before discarding this future. We can
  // also make sure it's non-blocking and will close-on-exec. Start by
  // checking we've got a "valid" file descriptor before dup'ing.
  if (fd < 0) {
    return Failure(os::strerror(EBADF));
  }

  fd = dup(fd);
  if (fd == -1) {
    return Failure(ErrnoError("Failed to duplicate file descriptor"));
  }

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

  return internal::_write(fd, Owned<string>(new string(data)), 0)
    .onAny(lambda::bind(&os::close, fd));
}


Future<Nothing> redirect(int from, Option<int> to, size_t chunk)
{
  // Make sure we've got "valid" file descriptors.
  if (from < 0 || (to.isSome() && to.get() < 0)) {
    return Failure(os::strerror(EBADF));
  }

  if (to.isNone()) {
    // Open up /dev/null that we can splice into.
    Try<int> open = os::open("/dev/null", O_WRONLY | O_CLOEXEC);

    if (open.isError()) {
      return Failure("Failed to open /dev/null for writing: " + open.error());
    }

    to = open.get();
  } else {
    // Duplicate 'to' so that we're in control of its lifetime.
    int fd = dup(to.get());
    if (fd == -1) {
      return Failure(ErrnoError("Failed to duplicate 'to' file descriptor"));
    }

    to = fd;
  }

  CHECK_SOME(to);

  // Duplicate 'from' so that we're in control of its lifetime.
  from = dup(from);
  if (from == -1) {
    os::close(to.get());
    return Failure(ErrnoError("Failed to duplicate 'from' file descriptor"));
  }

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

  return internal::splice(from, to.get(), chunk)
    .onAny(lambda::bind(&os::close, from))
    .onAny(lambda::bind(&os::close, to.get()));
}


// TODO(hartem): Most of the boilerplate code here is the same as
// in io::read, so this needs to be refactored.
Future<string> peek(int fd, size_t limit)
{
  process::initialize();

  if (limit > BUFFERED_READ_SIZE) {
    return Failure("Expected the number of bytes to be less than " +
                   stringify(BUFFERED_READ_SIZE));
  }

  // TODO(benh): Wrap up this data as a struct, use 'Owner'.
  boost::shared_array<char> data(new char[BUFFERED_READ_SIZE]);

  return io::peek(fd, data.get(), BUFFERED_READ_SIZE, limit)
    .then([=](size_t length) -> Future<string> {
      // At this point we have to return whatever data we were able to
      // peek, because we can not rely on peeking across message
      // boundaries.
      return string(data.get(), length);
    });
}

} // namespace io {
} // namespace process {
