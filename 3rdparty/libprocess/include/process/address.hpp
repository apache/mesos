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

#ifndef __PROCESS_ADDRESS_HPP__
#define __PROCESS_ADDRESS_HPP__

#include <stddef.h>
#include <stdint.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#include <glog/logging.h>

#ifndef __WINDOWS__
#include <sys/un.h>
#endif // __WINDOWS__

#include <ostream>
#include <utility>

#include <boost/functional/hash.hpp>

#include <stout/abort.hpp>
#include <stout/check.hpp>
#include <stout/ip.hpp>
#include <stout/variant.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

namespace process {
namespace network {

class Address;

namespace inet {

class Address
{
public:
  Address(const net::IP& _ip, uint16_t _port)
    : ip(_ip), port(_port) {}

  /**
   * Returns the hostname of this address's IP,
   * using a reverse DNS lookup for remote addresses
   * or the local hostname for a local address.
   *
   * @returns the hostname of this address's IP.
   */
  // TODO(jmlvanre): Consider making this return a Future in order to
  // deal with slow name resolution.
  //
  // TODO(bevers): A given IP can have multiple associated PTR records,
  // (e.g. a shared web server hosting multiple domains), so the return
  // value should probably be a list of strings.
  Try<std::string> lookup_hostname() const
  {
    const Try<std::string> hostname = ip.isAny()
      ? net::hostname()
      : net::getHostname(ip);

    if (hostname.isError()) {
      return Error(hostname.error());
    }

    return hostname.get();
  }

  operator sockaddr_storage() const
  {
    union {
      sockaddr_storage storage;
      sockaddr_in in;
      sockaddr_in6 in6;
    } sockaddr;
    memset(&sockaddr.storage, 0, sizeof(sockaddr_storage));
    switch (ip.family()) {
      case AF_INET:
        sockaddr.in.sin_family = AF_INET;
        sockaddr.in.sin_addr = ip.in().get();
        sockaddr.in.sin_port = htons(port);
        break;
      case AF_INET6:
        sockaddr.in6.sin6_family = AF_INET6;
        sockaddr.in6.sin6_addr = ip.in6().get();
        sockaddr.in6.sin6_port = htons(port);
        break;
      default:
        ABORT("Unexpected family: " + stringify(ip.family()));
    }
    return sockaddr.storage;
  }

  bool operator<(const Address& that) const
  {
    if (ip == that.ip) {
      return port < that.port;
    } else {
      return ip < that.ip;
    }
  }

  bool operator>(const Address& that) const
  {
    if (ip == that.ip) {
      return port > that.port;
    } else {
      return ip > that.ip;
    }
  }

  bool operator==(const Address& that) const
  {
    return (ip == that.ip && port == that.port);
  }

  bool operator!=(const Address& that) const
  {
    return !(*this == that);
  }

  // TODO(benh): Consider using `sockaddr_storage` here like we do for
  // `unix::Address`. This will require changing all places that
  // either the `ip` or `port` field are currently used.
  net::IP ip;
  uint16_t port;
};


inline std::ostream& operator<<(std::ostream& stream, const Address& address)
{
  stream << address.ip << ":" << address.port;
  return stream;
}

} // namespace inet {


namespace inet4 {

class Address : public inet::Address
{
public:
  static Address LOOPBACK_ANY()
  {
    return Address(net::IPv4::LOOPBACK(), 0);
  }

  static Address ANY_ANY()
  {
    return Address(net::IPv4::ANY(), 0);
  }

  Address(const net::IPv4& ip, uint16_t port)
    : inet::Address(ip, port) {}

  Address(const sockaddr_in& in)
    : inet::Address(net::IPv4(in.sin_addr), ntohs(in.sin_port)) {}
};

} // namespace inet4 {


namespace inet6 {

class Address : public inet::Address
{
public:
  static Address LOOPBACK_ANY()
  {
    return Address(net::IPv6::LOOPBACK(), 0);
  }

  static Address ANY_ANY()
  {
    return Address(net::IPv6::ANY(), 0);
  }

  Address(const net::IPv6& ip, uint16_t port)
    : inet::Address(ip, port) {}

  Address(const sockaddr_in6& in6)
    : inet::Address(net::IPv6(in6.sin6_addr), ntohs(in6.sin6_port)) {}
};

} // namespace inet6 {


#ifndef __WINDOWS__
namespace unix {

class Address
{
public:
  static Try<Address> create(const std::string& path)
  {
    sockaddr_un un;

    const size_t PATH_LENGTH = sizeof(un.sun_path);

    if (path.length() >= PATH_LENGTH) {
      return Error("Path too long, must be less than " +
                   stringify(PATH_LENGTH) + " bytes");
    }

    un.sun_family = AF_UNIX;
    memcpy(un.sun_path, path.c_str(), path.length() + 1);

    return Address(
        un, path.length() + offsetof(struct sockaddr_un, sun_path) + 1);
  }

  // For pathname sockets, `length` can be omitted to signal that it should be
  // determined automatically from the string length of the null-terminated
  // string inside `sun_path`. For abstract and unnamed domain sockets, the
  // correct length must be passed manually. See `man(7) unix` on the details
  // how to compute the correct length. For functions that return a socket
  // address (`recvfrom()`, `getpeername()`, etc.) the returned length will be
  // set to the correct value during the call.
  Address(const sockaddr_un& un, Option<socklen_t> _length = None())
    : sockaddr() // Zero initialize.
  {
    sockaddr.un = un;

    if (_length.isNone()) {
      CHECK(un.sun_path[0] != 0)
        << "Cannot automatically determine size of abstract socket address";

      length = ::strlen(un.sun_path) + offsetof(sockaddr_un, sun_path) + 1;
    } else {
      CHECK(_length.get() <= sizeof(struct sockaddr_un));
      length = _length.get();
    }
  }

  size_t size() const
  {
    return length;
  }

  std::string path() const
  {
    return std::string(sockaddr.un.sun_path, path_length());
  }

  operator sockaddr_storage() const
  {
    return sockaddr.storage;
  }

  bool operator==(const Address& that) const
  {
    return length == that.length &&
      !memcmp(sockaddr.un.sun_path, that.sockaddr.un.sun_path, path_length());
  }

private:
  friend std::ostream& operator<<(
      std::ostream& stream,
      const Address& address);

  // Size of the address data inside `sun_path`, in bytes.
  // The length computations here are defined in `man(7) unix`.
  size_t path_length() const
  {
    if (length == sizeof(sa_family_t)) {
      // Unnamed socket.
      return 0;
    } else if (sockaddr.un.sun_path[0] == '\0') {
      // Abstract domain socket.
      return length - sizeof(sa_family_t);
    } else {
      // Pathname socket.
      return length - offsetof(struct sockaddr_un, sun_path) - 1;
    }
  }

  union {
    sockaddr_storage storage;
    sockaddr_un un;
  } sockaddr;

  // Size of this socket. Unlike TCP/IP sockets, this is not just a
  // compile time constant, but depends on the type of the socket
  // address and the path it contains.
  socklen_t length;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const Address& address)
{
  std::string path = address.path();
  if (!path.empty() && path[0] == '\0') {
    path[0] = '@';
  }
  return stream << path;
}

} // namespace unix {
#endif // __WINDOWS__


// Represents a network "address", subsuming the `struct addrinfo` and
// `struct sockaddr` that typically is used to encapsulate an address.
//
// TODO(jieyu): Move this class to stout.
class Address :
  public Variant<
#ifndef __WINDOWS__
  unix::Address,
#endif // __WINDOWS__
  inet4::Address,
  inet6::Address>
{
public:
  enum class Family {
#ifndef __WINDOWS__
    UNIX,
#endif // __WINDOWS__
    INET4,
    INET6
  };

  // The `length` is the size of this `struct sockaddr`. For `AF_INET`
  // and `AF_INET6` this parameters is ignored, since there is only
  // one possible value anyways.
  static Try<Address> create(
      const sockaddr_storage& storage,
      Option<socklen_t> length = None())
  {
    switch (storage.ss_family) {
#ifndef __WINDOWS__
      case AF_UNIX:
        // We need to know the length in addition to the address, to
        // distinguish between e.g. an unnamed socket and an abstract
        // socket whose name is a single null byte.
        if (length.isNone()) {
          return Error("Need length to create unix address from sockaddr");
        }
        return unix::Address((const sockaddr_un&) storage, *length);
#endif // __WINDOWS__
      case AF_INET:
        return inet4::Address((const sockaddr_in&) storage);
      case AF_INET6:
        return inet6::Address((const sockaddr_in6&) storage);
      default:
        return Error("Unsupported family: " + stringify(storage.ss_family));
    }
  }

  // The `length` is the size of the data pointed to by `struct sockaddr`.
  static Try<Address> create(
      const sockaddr* address,
      size_t length)
  {
    switch (address->sa_family) {
#ifndef __WINDOWS__
      case AF_UNIX:
        // We need to know the length in addition to the address, to
        // distinguish between e.g. an unnamed socket and an abstract
        // socket whose name is a single null byte.
        if (length > sizeof(struct sockaddr_un)) {
          return Error("Invalid size for AF_UNIX sockaddr: " +
                       stringify(length) + " actual vs " +
                       stringify(sizeof(struct sockaddr_un)) + " expected");
        }
        return unix::Address(*((const sockaddr_un*)address), length);
#endif // __WINDOWS__
      case AF_INET:
        if (length < sizeof(struct sockaddr_in)) {
          return Error("Invalid size for AF_INET sockaddr: " +
                       stringify(length) + " actual vs " +
                       stringify(sizeof(struct sockaddr_in)) + " expected");
        }
        return inet4::Address(*((const sockaddr_in*)address));
      case AF_INET6:
        if (length < sizeof(struct sockaddr_in6)) {
          return Error("Invalid size for AF_INET6 sockaddr: " +
                       stringify(length) + " actual vs " +
                       stringify(sizeof(struct sockaddr_in6)) + " expected");
        }
        return inet6::Address(*((const sockaddr_in6*)address));
      default:
        return Error("Unsupported family: " + stringify(address->sa_family));
    }
  }

  // Helper constructor for converting an `inet::Address`.
  Address(const inet::Address& address)
    : Address([](const Try<Address>& address) {
        // We expect our implementation of the cast operator to be
        // correct, hence `Address::create` should always succeed.
        CHECK_SOME(address);
        return address.get();
      }(Address::create((sockaddr_storage) address))) {}

#ifndef __WINDOWS__
  Address(unix::Address address)
    : Variant<
    unix::Address,
    inet4::Address,
    inet6::Address>(std::move(address)) {}
#endif // __WINDOWS__

  Address(inet4::Address address)
    : Variant<
#ifndef __WINDOWS__
    unix::Address,
#endif // __WINDOWS__
    inet4::Address,
    inet6::Address>(std::move(address)) {}

  Address(inet6::Address address)
    : Variant<
#ifndef __WINDOWS__
    unix::Address,
#endif // __WINDOWS__
    inet4::Address,
    inet6::Address>(std::move(address)) {}

  Family family() const
  {
    return visit(
#ifndef __WINDOWS__
        [](const unix::Address& address) {
          return Address::Family::UNIX;
        },
#endif // __WINDOWS__
        [](const inet4::Address& address) {
          return Address::Family::INET4;
        },
        [](const inet6::Address& address) {
          return Address::Family::INET6;
        });
  }

  // Returns the storage size depending on the family of this address.
  size_t size() const
  {
    return visit(
#ifndef __WINDOWS__
        [](const unix::Address& address) {
          return address.size();
        },
#endif // __WINDOWS__
        [](const inet4::Address& address) {
          return sizeof(sockaddr_in);
        },
        [](const inet6::Address& address) {
          return sizeof(sockaddr_in6);
        });
  }

  // Implicit cast for working with C interfaces.
  operator sockaddr_storage() const
  {
    return visit(
#ifndef __WINDOWS__
        [](const unix::Address& address) {
          return (sockaddr_storage) address;
        },
#endif // __WINDOWS__
        [](const inet4::Address& address) {
          return (sockaddr_storage) address;
        },
        [](const inet6::Address& address) {
          return (sockaddr_storage) address;
        });

    // TODO(benh): With C++14 generic lambdas:
    // return visit(
    //     [](const auto& address) {
    //       return (sockaddr_storage) address;
    //     });
  }
};


// Helper for converting between Address and other types.
template <typename AddressType>
Try<AddressType> convert(Try<Address>&& address);


// TODO(benh): With C++14 generic lambdas:
// template <typename AddressType>
// Try<AddressType> convert(Try<Address>&& address)
// {
//   if (address.isError()) {
//     return Error(address.error());
//   }

//   return address->visit(
//       [](const AddressType& address) -> Try<AddressType> {
//         return address;
//       },
//       [](const auto&) -> Try<AddressType> {
//         return Error("Unexpected address family");
//       });
// }


#ifndef __WINDOWS__
template <>
inline Try<unix::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  return address->visit(
      [](const unix::Address& address) -> Try<unix::Address> {
        return address;
      },
      [](const inet4::Address&) -> Try<unix::Address> {
        return Error("Unexpected address family");
      },
      [](const inet6::Address&) -> Try<unix::Address> {
        return Error("Unexpected address family");
      });
}
#endif // __WINDOWS__


template <>
inline Try<inet4::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  return address->visit(
#ifndef __WINDOWS__
      [](const unix::Address&) -> Try<inet4::Address> {
        return Error("Unexpected address family");
      },
#endif // __WINDOWS__
      [](const inet4::Address& address) -> Try<inet4::Address> {
        return address;
      },
      [](const inet6::Address&) -> Try<inet4::Address> {
        return Error("Unexpected address family");
      });
}


template <>
inline Try<inet6::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  return address->visit(
#ifndef __WINDOWS__
      [](const unix::Address&) -> Try<inet6::Address> {
        return Error("Unexpected address family");
      },
#endif // __WINDOWS__
      [](const inet4::Address&) -> Try<inet6::Address> {
        return Error("Unexpected address family");
      },
      [](const inet6::Address& address) -> Try<inet6::Address> {
        return address;
      });
}


// Explicit instantiation in order to be able to upcast an `inet4::`
// or `inet6::Address` to an `inet::Address`.
template <>
inline Try<inet::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  return address->visit(
#ifndef __WINDOWS__
      [](const unix::Address& address) -> Try<inet::Address> {
        return Error("Unexpected address family");
      },
#endif // __WINDOWS__
      [](const inet4::Address& address) -> Try<inet::Address> {
        return address;
      },
      [](const inet6::Address& address) -> Try<inet::Address> {
        return address;
      });

  // TODO(benh): With C++14 generic lambdas:
  // return address->visit(
  //     [](const inet4::Address& address) -> Try<inet::Address> {
  //       return address;
  //     },
  //     [](const inet6::Address& address) -> Try<inet::Address> {
  //       return address;
  //     },
  //     [](const auto& t) -> Try<inet::Address> {
  //       return Error("Unexpected address family");
  //     });
}


template <>
inline Try<Address> convert(Try<Address>&& address)
{
  return std::move(address);
}

} // namespace network {
} // namespace process {


namespace std {

template <>
struct hash<process::network::inet::Address>
{
  typedef size_t result_type;

  typedef process::network::inet::Address argument_type;

  result_type operator()(const argument_type& address) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, std::hash<net::IP>()(address.ip));
    boost::hash_combine(seed, address.port);
    return seed;
  }
};


template <>
struct hash<process::network::inet4::Address>
  : hash<process::network::inet::Address>
{};


template <>
struct hash<process::network::inet6::Address>
  : hash<process::network::inet::Address>
{};

} // namespace std {

#endif // __PROCESS_ADDRESS_HPP__
