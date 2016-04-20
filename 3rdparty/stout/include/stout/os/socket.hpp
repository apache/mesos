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

#ifndef __STOUT_OS_SOCKET_HPP__
#define __STOUT_OS_SOCKET_HPP__

#include <stout/error.hpp>
#include <stout/try.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/socket.hpp>
#else
#include <stout/os/posix/socket.hpp>
#endif // __WINDOWS__

namespace net {

// Returns a socket file descriptor for the specified options.
// NOTE: on OS X, the returned socket will have the SO_NOSIGPIPE option set.
inline Try<int> socket(int family, int type, int protocol)
{
  // TODO(dpravat): Since Windows sockets are 64bit values,
  // an additional patch is required to avoid the truncation.
  int s;
  if ((s = ::socket(family, type, protocol)) == -1) {
    return ErrnoError();
  }

#ifdef __APPLE__
  // Disable SIGPIPE via setsockopt because OS X does not support
  // the MSG_NOSIGNAL flag on send(2).
  const int enable = 1;
  if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(int)) == -1) {
    return ErrnoError();
  }
#endif // __APPLE__

  return s;
}

} // namespace net {

#endif // __STOUT_OS_SOCKET_HPP__
