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
namespace internal {

Try<std::shared_ptr<SocketImpl>> PollSocketImpl::create(int_fd s)
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

Future<int_fd> accept(int_fd fd)
{
  Try<int_fd> accepted = network::accept(fd);
  if (accepted.isError()) {
    return Failure(accepted.error());
  }

  int_fd s = accepted.get();
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

  Try<Address> address = network::address(s);
  if (address.isError()) {
    LOG_IF(INFO, VLOG_IS_ON(1)) << "Failed to get address: "
                                << address.error();
    os::close(s);
    return Failure("Failed to get address: " + address.error());
  }

  // Turn off Nagle (TCP_NODELAY) so pipelined requests don't wait.
  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  if (address->family() == Address::Family::INET) {
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
  }

  return s;
}

} // namespace internal {


Future<std::shared_ptr<SocketImpl>> PollSocketImpl::accept()
{
  return io::poll(get(), io::READ)
    .then(lambda::bind(&internal::accept, get()))
    .then([](int_fd s) -> Future<std::shared_ptr<SocketImpl>> {
      Try<std::shared_ptr<SocketImpl>> impl = create(s);
      if (impl.isError()) {
        os::close(s);
        return Failure("Failed to create socket: " + impl.error());
      }
      return impl.get();
    });
}


namespace internal {

Future<Nothing> connect(
    const std::shared_ptr<PollSocketImpl>& socket,
    const Address& to)
{
  // Now check that a successful connection was made.
  int opt;
  socklen_t optlen = sizeof(opt);
  int_fd s = socket->get();

  // NOTE: We cast to `char*` here because the function prototypes on Windows
  // use `char*` instead of `void*`.
  if (::getsockopt(
          s,
          SOL_SOCKET,
          SO_ERROR,
          reinterpret_cast<char*>(&opt),
          &optlen) < 0) {
    return Failure(
        SocketError("Failed to get status of connection to " + stringify(to)));
  }

  if (opt != 0) {
    return Failure(SocketError(opt, "Failed to connect to " + stringify(to)));
  }

  return Nothing();
}

} // namespace internal {


Future<Nothing> PollSocketImpl::connect(const Address& address)
{
  Try<Nothing, SocketError> connect = network::connect(get(), address);
  if (connect.isError()) {
    if (net::is_inprogress_error(connect.error().code)) {
      return io::poll(get(), io::WRITE)
        .then(lambda::bind(&internal::connect, shared(this), address));
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

Future<size_t> socket_send_data(
    const std::shared_ptr<PollSocketImpl>& impl,
    const char* data, size_t size)
{
  CHECK(size > 0);

  while (true) {
    ssize_t length = net::send(impl->get(), data, size, MSG_NOSIGNAL);

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
      return io::poll(impl->get(), io::WRITE)
        .then(lambda::bind(&internal::socket_send_data, impl, data, size));
    } else if (length <= 0) {
      // Socket error or closed.
      if (length < 0) {
        const string error = os::strerror(errno);
        VLOG(1) << "Socket error while sending: " << error;
        return Failure(ErrnoError("Socket send failed"));
      } else {
        VLOG(1) << "Socket closed while sending";
        return length;
      }
    } else {
      CHECK(length > 0);

      return length;
    }
  }
}


Future<size_t> socket_send_file(
    const std::shared_ptr<PollSocketImpl>& impl,
    int_fd fd,
    off_t offset,
    size_t size)
{
  CHECK(size > 0);

  while (true) {
    Try<ssize_t, SocketError> length =
      os::sendfile(impl->get(), fd, offset, size);

    if (length.isSome()) {
      CHECK(length.get() >= 0);
      if (length.get() == 0) {
        // Socket closed.
        VLOG(1) << "Socket closed while sending";
      }
      return length.get();
    }

    if (net::is_restartable_error(length.error().code)) {
      // Interrupted, try again now.
      continue;
    } else if (net::is_retryable_error(length.error().code)) {
      // Might block, try again later.
      return io::poll(impl->get(), io::WRITE)
        .then(lambda::bind(
            &internal::socket_send_file,
            impl,
            fd,
            offset,
            size));
    } else {
      // Socket error or closed.
      VLOG(1) << length.error().message;
      return Failure(length.error());
    };
  }
}

} // namespace internal {


Future<size_t> PollSocketImpl::send(const char* data, size_t size)
{
  return io::poll(get(), io::WRITE)
    .then(lambda::bind(
        &internal::socket_send_data,
        shared(this),
        data,
        size));
}


Future<size_t> PollSocketImpl::sendfile(int_fd fd, off_t offset, size_t size)
{
  return io::poll(get(), io::WRITE)
    .then(lambda::bind(
        &internal::socket_send_file,
        shared(this),
        fd,
        offset,
        size));
}

} // namespace internal {
} // namespace network {
} // namespace process {
