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

#ifndef __PROCESS_NETWORK_HPP__
#define __PROCESS_NETWORK_HPP__

#include <process/address.hpp>

#include <stout/net.hpp>
#include <stout/try.hpp>

#include <stout/os/socket.hpp>


namespace process {
namespace network {

using net::socket;


// TODO(benh): Remove and defer to Socket::accept.
inline Try<int_fd> accept(int_fd s)
{
  sockaddr_storage storage;
  socklen_t length = sizeof(storage);

  int_fd accepted = net::accept(s, (sockaddr*) &storage, &length);
  if (accepted < 0) {
    return ErrnoError("Failed to accept");
  }

  return accepted;
}


// TODO(benh): Remove and defer to Socket::bind.
inline Try<Nothing> bind(int_fd s, const Address& address)
{
  sockaddr_storage storage = address;

  const socklen_t address_size = static_cast<socklen_t>(address.size());
  if (net::bind(s, (sockaddr*) &storage, address_size) < 0) {
    return ErrnoError("Failed to bind on " + stringify(address));
  }

  return Nothing();
}


// TODO(benh): Remove and defer to Socket::connect.
inline Try<Nothing, SocketError> connect(int_fd s, const Address& address)
{
  sockaddr_storage storage = address;

  const socklen_t address_size = static_cast<socklen_t>(address.size());
  if (net::connect(s, (sockaddr*) &storage, address_size) < 0) {
    return SocketError("Failed to connect to " + stringify(address));
  }

  return Nothing();
}


/**
 * Returns the `Address` with the assigned ip and assigned port.
 *
 * @return An `Address` or an error if the `getsockname` system call
 *     fails or the family type is not supported.
 */
inline Try<Address> address(int_fd s)
{
  sockaddr_storage storage;
  socklen_t length = sizeof(storage);

  if (::getsockname(s, (sockaddr*)&storage, &length) < 0) {
    return ErrnoError("Failed to getsockname");
  }

  return Address::create(storage);
}


/**
 * Returns the peer's `Address` for the accepted or connected socket.
 *
 * @return An `Address` or an error if the `getpeername` system call
 *     fails or the family type is not supported.
 */
inline Try<Address> peer(int_fd s)
{
  sockaddr_storage storage;
  socklen_t length = sizeof(storage);

  if (::getpeername(s, (sockaddr*)&storage, &length) < 0) {
    return ErrnoError("Failed to getpeername");
  }

  return Address::create(storage);
}

} // namespace network {
} // namespace process {

#endif // __PROCESS_NETWORK_HPP__
