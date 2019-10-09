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

#include <memory>
#include <string>

#include <boost/shared_array.hpp>

#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/network.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <process/ssl/flags.hpp>

#include <stout/os.hpp>
#include <stout/unreachable.hpp>

#ifdef USE_SSL_SOCKET
#ifdef USE_LIBEVENT
#include "posix/libevent/libevent_ssl_socket.hpp"
#else
#include "ssl/openssl_socket.hpp"
#endif // USE_LIBEVENT
#endif // USE_SSL_SOCKET

#include "poll_socket.hpp"

using std::string;

namespace process {
namespace network {
namespace internal {

Try<std::shared_ptr<SocketImpl>> SocketImpl::create(int_fd s, Kind kind)
{
  switch (kind) {
    case Kind::POLL:
      return PollSocketImpl::create(s);
#ifdef USE_SSL_SOCKET
    case Kind::SSL:
#ifdef USE_LIBEVENT
      return LibeventSSLSocketImpl::create(s);
#else
      return OpenSSLSocketImpl::create(s);
#endif // USE_LIBEVENT
#endif // USE_SSL_SOCKET
  }
  UNREACHABLE();
}


Try<std::shared_ptr<SocketImpl>> SocketImpl::create(
    Address::Family family,
    Kind kind)
{
  int domain = [=]() {
    switch (family) {
      case Address::Family::INET4: return AF_INET;
      case Address::Family::INET6: return AF_INET6;
#ifndef __WINDOWS__
      case Address::Family::UNIX: return AF_UNIX;
#endif // __WINDOWS__
    }
    UNREACHABLE();
  }();

  // Supported in Linux >= 2.6.27.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  Try<int_fd> s =
    network::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

  if (s.isError()) {
    return Error("Failed to create socket: " + s.error());
  }
#else
  Try<int_fd> s = network::socket(domain, SOCK_STREAM, 0);
  if (s.isError()) {
    return Error("Failed to create socket: " + s.error());
  }

  Try<Nothing> async = io::prepare_async(s.get());
  if (async.isError()) {
    os::close(s.get());
    return Error("Failed to create socket, prepare_async: " + async.error());
  }

  Try<Nothing> cloexec = os::cloexec(s.get());
  if (cloexec.isError()) {
    os::close(s.get());
    return Error("Failed to create socket, cloexec: " + cloexec.error());
  }
#endif

  Try<std::shared_ptr<SocketImpl>> impl = create(s.get(), kind);
  if (impl.isError()) {
    os::close(s.get());
  }

  return impl;
}


SocketImpl::Kind SocketImpl::DEFAULT_KIND()
{
  // NOTE: Some tests may change the OpenSSL flags and reinitialize
  // libprocess. In non-test code, the return value should be constant.
#ifdef USE_SSL_SOCKET
  return network::openssl::flags().enabled ? Kind::SSL : Kind::POLL;
#else
  return Kind::POLL;
#endif
}


Try<Address> SocketImpl::address() const
{
  // TODO(benh): Cache this result so that we don't have to make
  // unnecessary system calls each time.
  return network::address(get());
}


Try<Address> SocketImpl::peer() const
{
  // TODO(benh): Cache this result so that we don't have to make
  // unnecessary system calls each time.
  return network::peer(get());
}


Try<Address> SocketImpl::bind(const Address& address)
{
  Try<Nothing> bind = network::bind(get(), address);
  if (bind.isError()) {
    return Error(bind.error());
  }

  // Lookup and store assigned IP and assigned port.
  return network::address(get());
}


Future<string> SocketImpl::recv(const Option<ssize_t>& size)
{
  // Extend lifetime by holding onto a reference to ourself!
  auto self = shared_from_this();

  // Default chunk size to attempt to receive when nothing is
  // specified represents roughly 16 pages.
  static const size_t DEFAULT_CHUNK = 16 * os::pagesize();

  const size_t chunk = (size.isNone() || size.get() < 0)
    ? DEFAULT_CHUNK
    : size.get();

  boost::shared_array<char> data(new char[chunk]);
  string buffer;

  return loop(
      None(),
      [=]() {
        return self->recv(data.get(), chunk);
      },
      [=](size_t length) mutable -> ControlFlow<string> {
        if (length == 0) { // EOF.
          // Return everything we've received thus far, a subsequent
          // receive will return an empty string.
          return Break(std::move(buffer));
        }

        buffer.append(data.get(), length);

        if (size.isNone()) {
          // We've been asked just to return any data that we receive!
          return Break(std::move(buffer));
        } else if (size.get() < 0) {
          // We've been asked to receive until EOF so keep receiving
          // since according to the 'length == 0' check above we
          // haven't reached EOF yet.
          return Continue();
        } else if (
            static_cast<string::size_type>(size.get()) > buffer.size()) {
          // We've been asked to receive a particular amount of data and we
          // haven't yet received that much data so keep receiving.
          return Continue();
        }

        // We've received as much data as requested, so return that data!
        return Break(std::move(buffer));
      });
}


Future<Nothing> SocketImpl::send(const string& data)
{
  // Extend lifetime by holding onto a reference to ourself!
  auto self = shared_from_this();

  // We need to share the `index` between both lambdas below.
  std::shared_ptr<size_t> index(new size_t(0));

  // We store `data.size()` so that we won't make a copy of `data` in
  // each lambda below since some `data` might be very big!
  const size_t size = data.size();

  return loop(
      None(),
      [=]() {
        return self->send(data.data() + *index, size - *index);
      },
      [=](size_t length) -> ControlFlow<Nothing> {
        if ((*index += length) != size) {
          return Continue();
        }
        return Break();
      });
}

} // namespace internal {
} // namespace network {
} // namespace process {
