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

#ifndef __STOUT_OS_POSIX_FCNTL_HPP__
#define __STOUT_OS_POSIX_FCNTL_HPP__

#include <fcntl.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


// TODO(jieyu): Consider introducing a general os::fcntl function
// which allows us to get/set multiple flags in one call.
namespace os {

inline Try<Nothing> cloexec(int fd)
{
  int flags = ::fcntl(fd, F_GETFD);

  if (flags == -1) {
    return ErrnoError();
  }

  if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> unsetCloexec(int fd)
{
  int flags = ::fcntl(fd, F_GETFD);

  if (flags == -1) {
    return ErrnoError();
  }

  if (::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<bool> isCloexec(int fd)
{
  int flags = ::fcntl(fd, F_GETFD);

  if (flags == -1) {
    return ErrnoError();
  }

  return (flags & FD_CLOEXEC) != 0;
}


inline Try<Nothing> nonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return ErrnoError();
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<bool> isNonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return ErrnoError();
  }

  return (flags & O_NONBLOCK) != 0;
}

} // namespace os {

#endif // __STOUT_OS_POSIX_FCNTL_HPP__
