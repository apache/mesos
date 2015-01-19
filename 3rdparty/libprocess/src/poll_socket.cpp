#include <netinet/tcp.h>

#include <process/io.hpp>
#include <process/socket.hpp>

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
  Try<int> accepted = network::accept(fd, AF_INET);
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
  int on = 1;
  if (setsockopt(s, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    const char* error = strerror(errno);
    VLOG(1) << "Failed to turn off the Nagle algorithm: " << error;
    os::close(s);
    return Failure(
      "Failed to turn off the Nagle algorithm: " + stringify(error));
  }

  Try<Socket> socket = Socket::create(Socket::DEFAULT_KIND(), s);
  if (socket.isError()) {
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

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0 || opt != 0) {
    // Connect failure.
    VLOG(1) << "Socket error while connecting";
    return Failure("Socket error while connecting");
  }

  return Nothing();
}

} // namespace internal {


Future<Nothing> PollSocketImpl::connect(const Node& node)
{
  Try<int> connect = network::connect(get(), node);
  if (connect.isError()) {
    if (errno == EINPROGRESS) {
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

    if (length < 0 && (errno == EINTR)) {
      // Interrupted, try again now.
      continue;
    } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Might block, try again later.
      return io::poll(s, io::WRITE)
        .then(lambda::bind(&internal::socket_send_data, s, data, size));
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const char* error = strerror(errno);
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
        const char* error = strerror(errno);
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
