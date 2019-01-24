// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_WRITE_HPP__
#define __STOUT_OS_WRITE_HPP__

#include <cstring>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>
#include <stout/os/fsync.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/socket.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/write.hpp>
#else
#include <stout/os/posix/write.hpp>
#endif // __WINDOWS__

namespace os {

namespace signal_safe {

inline ssize_t write_impl(int_fd fd, const char* buffer, size_t count)
{
  size_t offset = 0;

  while (offset < count) {
    ssize_t length = os::write(fd, buffer + offset, count - offset);

    if (length < 0) {
#ifdef __WINDOWS__
      // NOTE: There is no actual difference between `WSAGetLastError` and
      // `GetLastError`, the former is an alias for the latter. So we can
      // simply use the former here for both `HANDLE` and `SOCKET` types of
      // `int_fd`. See MESOS-8764.
      int error = ::GetLastError();
#else
      int error = errno;
#endif // __WINDOWS__

      // TODO(benh): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK).
      if (net::is_restartable_error(error)) {
        continue;
      }
      return -1;
    }

    offset += length;
  }

  return offset;
}


inline ssize_t write(int_fd fd, const char* message)
{
  // `strlen` declared as async-signal safe in POSIX.1-2016 standard.
  return write_impl(fd, message, std::strlen(message));
}


inline ssize_t write(int_fd fd, const std::string& message)
{
  return write_impl(fd, message.data(), message.length());
}


template <typename T, typename... Args>
inline ssize_t write(int_fd fd, const T& message, Args... args)
{
  ssize_t result = write(fd, message);
  if (result < 0) {
    return result;
  }

  return write(fd, args...);
}

} // namespace signal_safe {


// Write out the string to the file at the current fd position.
inline Try<Nothing> write(int_fd fd, const std::string& message)
{
  ssize_t result = signal_safe::write(fd, message);
  if (result < 0) {
#ifdef __WINDOWS__
    return WindowsError();
#else
    return ErrnoError();
#endif // __WINDOWS__
  }

  return Nothing();
}


// A wrapper function for the above `write()` with opening and closing the file.
// If `sync` is set to true, an `fsync()` will be called before `close()`.
inline Try<Nothing> write(
    const std::string& path,
    const std::string& message,
    bool sync = false)
{
  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error(fd.error());
  }

  Try<Nothing> write = os::write(fd.get(), message);

  if (sync && write.isSome()) {
    // We call `fsync()` before closing the file instead of opening it with the
    // `O_SYNC` flag for better performance. See:
    // http://lkml.iu.edu/hypermail/linux/kernel/0105.3/0353.html
    write = os::fsync(fd.get());
  }

  Try<Nothing> close = os::close(fd.get());

  // We propagate `close` failures if `write` on the file was successful.
  if (write.isSome() && close.isError()) {
    write =
      Error("Failed to close '" + stringify(fd.get()) + "':" + close.error());
  }

  return write;
}


// NOTE: This overload is necessary to disambiguate between arguments
// of type `HANDLE` (`typedef void*`) and `char*` on Windows.
inline Try<Nothing> write(
    const char* path,
    const std::string& message,
    bool sync = false)
{
  return write(std::string(path), message, sync);
}

} // namespace os {


#endif // __STOUT_OS_WRITE_HPP__
