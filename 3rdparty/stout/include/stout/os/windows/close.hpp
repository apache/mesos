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

#ifndef __STOUT_OS_WINDOWS_CLOSE_HPP__
#define __STOUT_OS_WINDOWS_CLOSE_HPP__

#include <errno.h>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/socket.hpp>

namespace os {
namespace internal {

// When a debugger is attached to the process, `::CloseHandle` can throw a
// structured exception under some circumstances (for example, when we pass it
// an invalid handle to close). This function will catch the exception so that
// our test runs don't get interrupted when we happen to run them with a
// debugger.
//
// NOTE: Structured exceptions emitted by the C runtime APIs can't be caught
// using C++ exceptions by default, and enabling this requires adding special
// build flags. Since we don't gain anything by using this feature, we choose
// to just use the default behavior. Note also that structured exceptions must
// be caught in functions that do not require object unwinding (e.g., no
// destructors need to be called), so we make it a standalone, isolated
// function.
inline bool safe_closehandle(HANDLE handle)
{
  BOOL closed = false;
  __try
  {
    closed = ::CloseHandle(handle);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    ::SetLastError(ERROR_INVALID_HANDLE);
  }

  return static_cast<bool>(closed);
}


// A version of the POSIX-standard function, `close`, that catches structured
// exceptions thrown by the CRT. In these cases, we instead return `-1` and set
// `errno` to `EBADF`.
//
// `safe_close` is intended as an important, last-ditch attempt to avoid
// throwing a process-halting exception if we accidentally call `_close` on a
// file descriptor that corresponds to a socket. There are checks meant to
// prevent this in the `os::close` family as well; this is just our last
// defense.
//
// NOTE: Structured exceptions emitted by the C runtime APIs can't be caught
// using C++ exceptions by default, and enabling this requires adding special
// build flags. Since we don't gain anything by using this feature, we choose
// to just use the default behavior. Note also that structured exceptions must
// be caught in functions that do not require object unwinding (e.g., no
// destructors need to be called), so we make it a standalone, isolated
// function.
inline int safe_close(int fd)
{
  int close = -1;
  __try
  {
    close = ::_close(fd);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    ::_set_errno(EBADF);
  }

  return close;
}

} // namespace internal {


// NOTE: This function needs to be defined before `os::close(int)`. The `int`
// version checks whether its argument is a socket, and if it is, it makes uses
// `static_cast<SOCKET>` to dispatch to this version of the function.
inline Try<Nothing> close(SOCKET socket)
{
  if (::closesocket(socket) == SOCKET_ERROR) {
    return WindowsSocketError("os::close: Failed to close socket");
  }

  return Nothing();
}


inline Try<Nothing> close(int fd)
{
  // NOTE: This is a defensive measure. Since `int` is used throughout the
  // codebase to represent file descriptors, including those which refer to
  // sockets, it is possible that we could accidentally cast a `SOCKET` to an
  // `int` and pass it in here. Thus, we check explicitly whether `fd` is a
  // socket, and dispatch to the appropriate `close` function.
  //
  // The most notable example of this problem is `socket.hpp`, which represents
  // the socket using an `int`. Changing this would require major surgery
  // throughout the codebase.
  //
  // Note also that casting a file descriptor to a `SOCKET`, however, is less
  // likely. Hence it is only necessary to cast in this variant of the function.
  //
  // TODO(hausdorff): Transition away from this defensive measure when we
  // transition from `int` to an encapsulated file descriptor type.
  if (net::is_socket(static_cast<SOCKET>(fd))) {
    return close(static_cast<SOCKET>(fd));
  } else {
    if (::_close(fd) == -1) {
      return ErrnoError("os::close: Failed to close file descriptor");
    }
  }

  return Nothing();
}


inline Try<Nothing> close(HANDLE handle)
{
  if (!internal::safe_closehandle(handle)) {
    return WindowsError("os::close: Failed to close handle");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_CLOSE_HPP__
