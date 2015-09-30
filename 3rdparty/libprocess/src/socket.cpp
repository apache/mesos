/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <unistd.h> // For sysconf.

#include <memory>
#include <string>

#include <boost/shared_array.hpp>

#include <process/network.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#ifdef USE_SSL_SOCKET
#include "libevent_ssl_socket.hpp"
#include "openssl.hpp"
#endif
#include "poll_socket.hpp"

using std::string;

namespace process {
namespace network {

Try<Socket> Socket::create(Kind kind, Option<int> s)
{
  // If the caller passed in a file descriptor, we do
  // not own its life cycle and must not close it.
  bool owned = s.isNone();

  if (owned) {
    // Supported in Linux >= 2.6.27.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    Try<int> fd =
      network::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd.isError()) {
      return Error("Failed to create socket: " + fd.error());
    }
#else
    Try<int> fd = network::socket(AF_INET, SOCK_STREAM, 0);
    if (fd.isError()) {
      return Error("Failed to create socket: " + fd.error());
    }

    Try<Nothing> nonblock = os::nonblock(fd.get());
    if (nonblock.isError()) {
      os::close(fd.get());
      return Error("Failed to create socket, nonblock: " + nonblock.error());
    }

    Try<Nothing> cloexec = os::cloexec(fd.get());
    if (cloexec.isError()) {
      os::close(fd.get());
      return Error("Failed to create socket, cloexec: " + cloexec.error());
    }
#endif

    s = fd.get();
  }

  switch (kind) {
    case POLL: {
      Try<std::shared_ptr<Socket::Impl>> socket =
        PollSocketImpl::create(s.get());
      if (socket.isError()) {
        if (owned) {
          os::close(s.get());
        }
        return Error(socket.error());
      }
      return Socket(socket.get());
    }
#ifdef USE_SSL_SOCKET
    case SSL: {
      Try<std::shared_ptr<Socket::Impl>> socket =
        LibeventSSLSocketImpl::create(s.get());
      if (socket.isError()) {
        if (owned) {
          os::close(s.get());
        }
        return Error(socket.error());
      }
      return Socket(socket.get());
    }
#endif
    // By not setting a default we leverage the compiler errors when
    // the enumeration is augmented to find all the cases we need to
    // provide.
  }
}


const Socket::Kind& Socket::DEFAULT_KIND()
{
  static const Kind DEFAULT =
#ifdef USE_SSL_SOCKET
      network::openssl::flags().enabled ? Socket::SSL : Socket::POLL;
#else
      Socket::POLL;
#endif

  return DEFAULT;
}


Try<Address> Socket::Impl::address() const
{
  // TODO(benh): Cache this result so that we don't have to make
  // unnecessary system calls each time.
  return network::address(get());
}


Try<Address> Socket::Impl::peer() const
{
  // TODO(benh): Cache this result so that we don't have to make
  // unnecessary system calls each time.
  return network::peer(get());
}


Try<Address> Socket::Impl::bind(const Address& address)
{
  Try<int> bind = network::bind(get(), address);
  if (bind.isError()) {
    return Error(bind.error());
  }

  // Lookup and store assigned IP and assigned port.
  return network::address(get());
}


static Future<string> _recv(
    Socket socket,
    const Option<ssize_t>& size,
    Owned<string> buffer,
    size_t chunk,
    boost::shared_array<char> data,
    size_t length)
{
  if (length == 0) { // EOF.
    // Return everything we've received thus far, a subsequent receive
    // will return an empty string.
    return string(*buffer);
  }

  buffer->append(data.get(), length);

  if (size.isNone()) {
    // We've been asked just to return any data that we receive!
    return string(*buffer);
  } else if (size.get() < 0) {
    // We've been asked to receive until EOF so keep receiving since
    // according to the 'length == 0' check above we haven't reached
    // EOF yet.
    return socket.recv(data.get(), chunk)
      .then(lambda::bind(&_recv,
                         socket,
                         size,
                         buffer,
                         chunk,
                         data,
                         lambda::_1));
  } else if (size.get() > buffer->size()) {
    // We've been asked to receive a particular amount of data and we
    // haven't yet received that much data so keep receiving.
    return socket.recv(data.get(), size.get() - buffer->size())
      .then(lambda::bind(&_recv,
                         socket,
                         size,
                         buffer,
                         chunk,
                         data,
                         lambda::_1));
  }

  // We've received as much data as requested, so return that data!
  return string(*buffer);
}


Future<string> Socket::Impl::recv(const Option<ssize_t>& size)
{
  // Default chunk size to attempt to receive when nothing is
  // specified represents roughly 16 pages.
  static const size_t DEFAULT_CHUNK = 16 * sysconf(_SC_PAGESIZE);

  size_t chunk = (size.isNone() || size.get() < 0)
    ? DEFAULT_CHUNK
    : size.get();

  Owned<string> buffer(new string());
  boost::shared_array<char> data(new char[chunk]);

  return recv(data.get(), chunk)
    .then(lambda::bind(&_recv,
                       socket(),
                       size,
                       buffer,
                       chunk,
                       data,
                       lambda::_1));
}


static Future<Nothing> _send(
    Socket socket,
    Owned<string> data,
    size_t index,
    size_t length)
{
  // Increment the index into the data.
  index += length;

  // Check if we've sent all of the data.
  if (index == data->size()) {
    return Nothing();
  }

  // Keep sending!
  return socket.send(data->data() + index, data->size() - index)
    .then(lambda::bind(&_send, socket, data, index, lambda::_1));
}


Future<Nothing> Socket::Impl::send(const std::string& _data)
{
  Owned<string> data(new string(_data));

  return send(data->data(), data->size())
    .then(lambda::bind(&_send, socket(), data, 0, lambda::_1));
}


} // namespace network {
} // namespace process {
