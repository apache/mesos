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

#ifndef __STOUT_OS_POSIX_SOCKET_HPP__
#define __STOUT_OS_POSIX_SOCKET_HPP__

#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

namespace net {

// Import `socket` functions into `net::` namespace.
using ::accept;
using ::bind;
using ::connect;
using ::recv;
using ::send;

// The error indicates the last socket operation has been
// interupted, the operation can be restarted immediately.
inline bool is_restartable_error(int error)
{
  return (error == EINTR);
}


// The error indicates the last socket function on a non-blocking socket
// cannot be completed. This is a temporary condition and the caller can
// retry the operation later.
inline bool is_retryable_error(int error)
{
  return (error == EWOULDBLOCK || error == EAGAIN);
}


inline bool is_inprogress_error(int error)
{
  return (error == EINPROGRESS);
}


inline bool is_socket(int fd)
{
  struct stat statbuf;
  if (::fstat(fd, &statbuf) < 0) {
    return false;
  }

  return S_ISSOCK(statbuf.st_mode) != 0;
}

} // namespace net {

#endif // __STOUT_OS_POSIX_SOCKET_HPP__
