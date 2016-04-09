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


#ifdef __WINDOWS__
#include <stout/windows.hpp>
#else
#include <netinet/tcp.h>
#endif // __WINDOWS__

#include <process/io.hpp>
#include <process/network.hpp>
#include <process/socket.hpp>

#include <stout/os/sendfile.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os.hpp>

#include "config.hpp"
#include "poll_socket.hpp"

using std::string;

namespace process {
namespace network {

Try<std::shared_ptr<Socket::Impl>> PollSocketImpl::create(int s)
{
  return std::make_shared<PollSocketImpl>(s);
}


Try<Nothing> PollSocketImpl::listen(int backlog)
{
  if (::listen(get(), backlog) < 0) {
    return ErrnoError();
  }
  return Nothing();
}


namespace internal {

Future<Socket> accept(int fd)
{
  Try<int> accepted = network::accept(fd);
  if (accepted.isError()) {
    return Failure(accepted.error());
  }

  int s = accepted.get();
  Try<Nothing> nonblock = os::nonblock(s);
  if (nonblock.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to accept, nonblock: "
                                << nonblock.error();
    os::close(s);
    return Failure("Failed to accept, nonblock: " + nonblock.error());
  }

  Try<Nothing> cloexec = os::cloexec(s);
  if (cloexec.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to accept, cloexec: "
                                << cloexec.error();
    os::close(s);
    return Failure("Failed to accept, cloexec: " + cloexec.error());
  }

  // Turn off Nagle (TCP_NODELAY) so pipelined requests don't wait.
  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  int on = 1;
  if (::setsockopt(
          s,
          SOL_TCP,
          TCP_NODELAY,
          reinterpret_cast<const char*>(&on),
          sizeof(on)) < 0) {
    const string error = os::strerror(errno);
    VLOG(1) << "Failed to turn off the Nagle algorithm: " << error;
    os::close(s);
    return Failure(
      "Failed to turn off the Nagle algorithm: " + stringify(error));
  }

  Try<Socket> socket = Socket::create(Socket::DEFAULT_KIND(), s);
  if (socket.isError()) {
    os::close(s);
    return Failure("Failed to accept, create socket: " + socket.error());
  }
  return socket.get();
}

} // namespace internal {


Future<Socket> PollSocketImpl::accept()
{
  return io::poll(get(), io::READ)
    .then(lambda::bind(&internal::accept, get()));
}


namespace internal {

Future<Nothing> connect(const Socket& socket)
{
  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);
  int s = socket.get();

  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  if (::getsockopt(
          s,
          SOL_SOCKET,
          SO_ERROR,
          reinterpret_cast<char*>(&opt),
          &optlen) < 0 ||
      opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    return Failure("Socket error while connecting");
  }

  return Nothing();
}

} // namespace internal {


Future<Nothing> PollSocketImpl::connect(const Address& address)
{
  Try<int, SocketError> connect = network::connect(get(), address);
  if (connect.isError()) {
    if (net::is_inprogress_error(connect.error().code)) {
      return io::poll(get(), io::WRITE)
        .then(lambda::bind(&internal::connect, socket()));
    }

    return Failure(connect.error());
  }

  return Nothing();
}


Future<size_t> PollSocketImpl::recv(char* data, size_t size)
{
  return io::read(get(), data, size);
}


namespace internal {

Future<size_t> socket_send_data(int s, const char* data, size_t size)
{
  CHECK(size > 0);

  while (true) {
    ssize_t length = send(s, data, size, MSG_NOSIGNAL);

#ifdef __WINDOWS__
    int error = WSAGetLastError();
#else
    int error = errno;
#endif // __WINDOWS__

    if (length < 0 && net::is_restartable_error(error)) {
      // Interrupted, try again now.
      continue;
    } else if (length < 0 && net::is_retryable_error(error)) {
      // Might block, try again later.
      return io::poll(s, io::WRITE)
        .then(lambda::bind(&internal::socket_send_data, s, data, size));
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const string error = os::strerror(errno);
        VLOG(1) << "Socket error while sending: " << error;
      } else {
        VLOG(1) << "Socket closed while sending";
      }
      if (length == 0) {
        return length;
      } else {
        return Failure(ErrnoError("Socket send failed"));
      }
    } else {
      CHECK(length > 0);

      return length;
    }
  }
}


Future<size_t> socket_send_file(int s, int fd, off_t offset, size_t size)
{
  CHECK(size > 0);

  while (true) {
    ssize_t length = os::sendfile(s, fd, offset, size);

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      return io::poll(s, io::WRITE)
        .then(lambda::bind(&internal::socket_send_file, s, fd, offset, size));
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const string error = os::strerror(errno);
        VLOG(1) << "Socket error while sending: " << error;
      } else {
        VLOG(1) << "Socket closed while sending";
      }
      if (length == 0) {
        return length;
      } else {
        return Failure(ErrnoError("Socket sendfile failed"));
      }
    } else {
      CHECK(length > 0);

      return length;
    }
  }
}

} // namespace internal {


Future<size_t> PollSocketImpl::send(const char* data, size_t size)
{
  return io::poll(get(), io::WRITE)
    .then(lambda::bind(&internal::socket_send_data, get(), data, size));
}


Future<size_t> PollSocketImpl::sendfile(int fd, off_t offset, size_t size)
{
  return io::poll(get(), io::WRITE)
    .then(lambda::bind(&internal::socket_send_file, get(), fd, offset, size));
}

} // namespace network {
} // namespace process {
