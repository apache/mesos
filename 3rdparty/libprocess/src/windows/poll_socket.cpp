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


#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/network.hpp>
#include <process/socket.hpp>

#include <stout/windows.hpp>

#include <stout/os.hpp>
#include <stout/os/sendfile.hpp>
#include <stout/os/strerror.hpp>

#include "config.hpp"
#include "poll_socket.hpp"

#include "windows/libwinio.hpp"

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
  // Need to hold a copy of `this` so that we can detect if the underlying
  // socket has changed (i.e. closed) before we return from `io::poll`.
  std::weak_ptr<SocketImpl> weak_self(shared(this));

  Try<Address> address = network::address(get());
  if (address.isError()) {
    return Failure("Failed to get address: " + address.error());
  }

  int family = 0;
  if (address->family() == Address::Family::INET4) {
    family = AF_INET;
  } else if (address->family() == Address::Family::INET6) {
    family = AF_INET6;
  } else {
    return Failure("Unsupported address family. Windows only supports IP.");
  }

  Try<int_fd> accept_socket_ = net::socket(family, SOCK_STREAM, 0);
  if (accept_socket_.isError()) {
    return Failure(accept_socket_.error());
  }

  int_fd accept_socket = accept_socket_.get();

  return windows::accept(get(), accept_socket)
    .onAny([accept_socket](const Future<Nothing> future) {
      if (!future.isReady()) {
        os::close(accept_socket);
      }
    })
    .then([weak_self, accept_socket]() -> Future<std::shared_ptr<SocketImpl>> {
      std::shared_ptr<SocketImpl> self(weak_self.lock());
      if (self == nullptr) {
        return Failure("Socket destroyed while accepting");
      }

      SOCKET listen = self->get();

      // Inherit from the listening socket.
      int res = ::setsockopt(
          accept_socket,
          SOL_SOCKET,
          SO_UPDATE_ACCEPT_CONTEXT,
          reinterpret_cast<char*>(&listen),
          sizeof(listen));

      if (res != 0) {
        const WindowsError error;
        os::close(accept_socket);
        return Failure("Failed to set accepted socket: " + error.message);
      }

      // Disable Nagle algorithm, since we care about latency more than
      // throughput. See https://en.wikipedia.org/wiki/Nagle%27s_algorithm
      // for more info.
      const int on = 1;
      res = ::setsockopt(
          accept_socket,
          SOL_TCP,
          TCP_NODELAY,
          reinterpret_cast<const char*>(&on),
          sizeof(on));

      if (res != 0) {
        const WindowsError error;
        os::close(accept_socket);
        return Failure(
            "Failed to turn off the Nagle algorithm: " + error.message);
      }

      Try<Nothing> error = io::prepare_async(accept_socket);
      if (error.isError()) {
        os::close(accept_socket);
        return Failure(
            "Failed to set socket for asynchronous IO: " + error.error());
      }

      Try<std::shared_ptr<SocketImpl>> impl = create(accept_socket);
      if (impl.isError()) {
        os::close(accept_socket);
        return Failure("Failed to create socket: " + impl.error());
      }

      return impl.get();
    });
}


Future<Nothing> PollSocketImpl::connect(
    const Address& address)
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  return windows::connect(self->get(), address)
    .then([self]() -> Future<Nothing> {
      // After the async connect (`ConnectEx`) is called, the socket is in a
      // "default" state, which means it doesn't have the previously set
      // properties or options set. We need to set `SO_UPDATE_CONNECT_CONTEXT`
      // so that it regains its properties. For more information, see
      // https://msdn.microsoft.com/en-us/library/windows/desktop/ms737606(v=vs.85).aspx // NOLINT(whitespace/line_length)
      int res = ::setsockopt(
          self->get(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

      if (res != 0) {
        WindowsError error;
        return Failure("Failed to set connected socket: " + error.message);
      }

      return Nothing();
    });
}


#ifdef USE_SSL_SOCKET
Future<Nothing> PollSocketImpl::connect(
    const Address& address,
    const openssl::TLSClientConfig& config)
{
  LOG(FATAL) << "TLS config was passed to a PollSocket.";
}
#endif


Future<size_t> PollSocketImpl::recv(char* data, size_t size)
{
  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return from the call to
  // `io::read` and end up reading data incorrectly.
  auto self = shared(this);

  return io::read(get(), data, size).then([self](size_t length) {
    return length;
  });
}


Future<size_t> PollSocketImpl::send(const char* data, size_t size)
{
  CHECK(size > 0); // TODO(benh): Just return 0 if `size` is 0?

  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  // TODO(benh): Reuse `io::write`? Or is `net::send` and
  // `MSG_NOSIGNAL` critical here?
  return io::write(get(), data, size).then([self](size_t length) {
    return length;
  });
}


Future<size_t> PollSocketImpl::sendfile(int_fd fd, off_t offset, size_t size)
{
  CHECK(size > 0); // TODO(benh): Just return 0 if `size` is 0?

  // Need to hold a copy of `this` so that the underlying socket
  // doesn't end up getting reused before we return.
  auto self = shared(this);

  return windows::sendfile(self->get(), fd, offset, size)
    .then([self](size_t length) { return length; });
}

} // namespace internal {
} // namespace network {
} // namespace process {
