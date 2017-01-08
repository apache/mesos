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


Future<std::shared_ptr<SocketImpl>> PollSocketImpl::accept()
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return from the call to
  // `io::poll` and end up accepting a socket incorrectly.
  auto self = shared(this);

  return io::poll(get(), io::READ)
    .then([self]() -> Future<std::shared_ptr<SocketImpl>> {
      Try<int_fd> accepted = network::accept(self->get());
      if (accepted.isError()) {
        return Failure(accepted.error());
      }

      int_fd s = accepted.get();
      Try<Nothing> nonblock = os::nonblock(s);
      if (nonblock.isError()) {
        os::close(s);
        return Failure("Failed to accept, nonblock: " + nonblock.error());
      }

      Try<Nothing> cloexec = os::cloexec(s);
      if (cloexec.isError()) {
        os::close(s);
        return Failure("Failed to accept, cloexec: " + cloexec.error());
      }

      Try<Address> address = network::address(s);
      if (address.isError()) {
        os::close(s);
        return Failure("Failed to get address: " + address.error());
      }

      // Turn off Nagle (TCP_NODELAY) so pipelined requests don't wait.
      // NOTE: We cast to `char*` here because the function prototypes
      // on Windows use `char*` instead of `void*`.
      if (address->family() == Address::Family::INET4 ||
          address->family() == Address::Family::INET6) {
        int on = 1;
        if (::setsockopt(
                s,
                SOL_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&on),
                sizeof(on)) < 0) {
          const string error = os::strerror(errno);
          os::close(s);
          return Failure(
              "Failed to turn off the Nagle algorithm: " + stringify(error));
        }
      }

      Try<std::shared_ptr<SocketImpl>> impl = create(s);
      if (impl.isError()) {
        os::close(s);
        return Failure("Failed to create socket: " + impl.error());
      }

      return impl.get();
    });
}


Future<Nothing> PollSocketImpl::connect(const Address& address)
{
  Try<Nothing, SocketError> connect = network::connect(get(), address);
  if (connect.isError()) {
    if (net::is_inprogress_error(connect.error().code)) {
      // Need to hold a copy of `this` so that the underlying socket
      // doesn't end up getting reused before we return from the call
      // to `io::poll` and end up connecting incorrectly.
      auto self = shared(this);

      return io::poll(get(), io::WRITE)
        .then([self, address]() -> Future<Nothing> {
          // Now check that a successful connection was made.
          int opt;
          socklen_t optlen = sizeof(opt);

          // NOTE: We cast to `char*` here because the function
          // prototypes on Windows use `char*` instead of `void*`.
          if (::getsockopt(
                  self->get(),
                  SOL_SOCKET,
                  SO_ERROR,
                  reinterpret_cast<char*>(&opt),
                  &optlen) < 0) {
            return Failure(SocketError(
                "Failed to get status of connect to " + stringify(address)));
          }

          if (opt != 0) {
            return Failure(SocketError(
                opt,
                "Failed to connect to " +
                stringify(address)));
          }

          return Nothing();
        });
    }

    return Failure(connect.error());
  }

  return Nothing();
}


Future<size_t> PollSocketImpl::recv(char* data, size_t size)
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return from the call to
  // `io::read` and end up reading data incorrectly.
  auto self = shared(this);

  return io::read(get(), data, size)
    .then([self](size_t length) {
      return length;
    });
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
