#ifndef __STOUT_OS_SENDFILE_HPP__
#define __STOUT_OS_SENDFILE_HPP__

#include <errno.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif // __linux__
#ifdef __APPLE__
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#endif // __APPLE__

#ifdef __linux__
#include <stout/fatal.hpp>
#endif // __linux__
#include <stout/os/signals.hpp>

namespace os {

// Returns the amount of bytes written from the input file
// descriptor to the output socket. On error, returns -1 and
// errno indicates the error.
// NOTE: The following limitations exist because of the OS X
// implementation of sendfile:
//   1. s must be a stream oriented socket descriptor.
//   2. fd must be a regular file descriptor.
inline ssize_t sendfile(int s, int fd, off_t offset, size_t length)
{
#ifdef __linux__
  suppress (SIGPIPE) {
    // This will set errno to EPIPE if a SIGPIPE occurs.
    return ::sendfile(s, fd, &offset, length);
  }
  fatal("Unreachable statement");
  return -1;
#elif defined __APPLE__
  // On OS X, sendfile does not need to have SIGPIPE suppressed.
  off_t _length = static_cast<off_t>(length);

  if (::sendfile(fd, s, offset, &_length, NULL, 0) < 0) {
    if (errno == EAGAIN && _length > 0) {
      return _length;
    }
    return -1;
  }

  return _length;
#endif // __APPLE__
}

} // namespace os {

#endif // __STOUT_OS_SENDFILE_HPP__
