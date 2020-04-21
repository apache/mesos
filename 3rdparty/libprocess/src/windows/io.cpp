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

#include <string>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp> // For process::initialize.

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#include "io_internal.hpp"

#include "windows/libwinio.hpp"
#include "windows/event_loop.hpp"

namespace process {
namespace io {


Future<short> poll(int_fd fd, short events)
{
  if (events != io::READ) {
    return Failure("Expected io::READ (" + stringify(io::READ) + ")"
                   " but received " + stringify(events));
  }

  return io::internal::read(fd, nullptr, 0, false)
    .then([]() { return io::READ; });
}


namespace internal {

Future<size_t> read(int_fd fd, void* data, size_t size, bool bypassZeroRead)
{
  process::initialize();

  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0 && bypassZeroRead) {
    return 0;
  }

  // Just do a synchronous call.
  if (!fd.is_overlapped()) {
    ssize_t result = os::read(fd, data, size);
    if (result == -1) {
      return Failure(WindowsError().message);
    }
    return static_cast<size_t>(result);
  }

  return windows::read(fd, data, size);
}

Future<size_t> write(int_fd fd, const void* data, size_t size)
{
  process::initialize();

  // TODO(benh): Let the system calls do what ever they're supposed to
  // rather than return 0 here?
  if (size == 0) {
    return 0;
  }

  // Just do a synchronous call.
  if (!fd.is_overlapped()) {
    ssize_t result = os::write(fd, data, size);
    if (result == -1) {
      return Failure(WindowsError().message);
    }
    return static_cast<size_t>(result);
  }

  return windows::write(fd, data, size);
}


Try<Nothing> prepare_async(int_fd fd)
{
  if (fd.is_overlapped()) {
    // TODO(andschwa): Remove this check, see MESOS-9097.
    if (!libwinio_loop) {
      return Error("Windows IOCP event loop is not initialized");
    }

    return libwinio_loop->registerHandle(fd);
  }

  // For non-overlapped fds, we will only error if it's not a disk file, since
  // those are "fast" devices, so blocking IO should be okay. This is the same
  // as Linux's `O_NONBLOCK`, which doesn't work on regular files.
  if (fd.type() == os::WindowsFD::Type::SOCKET) {
    return Error("Got a non-overlapped socket");
  }

  DWORD type = ::GetFileType(fd);
  if (type != FILE_TYPE_DISK) {
    WindowsError error;
    return Error(std::string(
        "io::prepare_async only accepts disk files for non-overlapped files. "
        "Got type: ") + stringify(type) + " with possible error: " +
        error.message);
  }

  return Nothing();
}


Try<bool> is_async(int_fd fd)
{
  if (fd.is_overlapped()) {
    return fd.get_iocp() != nullptr;
  }

  // For non-overlapped fds, we only accept disk files, since they are "fast"
  // devices, similar to how Linux `O_NONBLOCK` no-ops on "fast" file types
  // like regular files.
  if (fd.type() == os::WindowsFD::Type::SOCKET) {
    return false;
  }

  DWORD type = ::GetFileType(fd);
  WindowsError error;
  if (type == FILE_TYPE_UNKNOWN && error.code != NO_ERROR) {
    return error;
  }

  return type == FILE_TYPE_DISK;
}

} // namespace internal {
} // namespace io {
} // namespace process {
