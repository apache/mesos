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

#include <boost/functional/hash.hpp>

#include <stout/abort.hpp>
#include <stout/check.hpp>
#include <stout/ip.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

namespace process {
namespace network {

namespace inet {
class Address;
} // namespace inet {

#ifndef __WINDOWS__
namespace unix {
class Address;
} // namespace unix {
#endif // __WINDOWS__

// Represents a network "address", subsuming the `struct addrinfo` and
// `struct sockaddr` that typically is used to encapsulate an address.
//
// TODO(jieyu): Move this class to stout.
class Address
{
public:
  enum class Family {
    INET,
#ifndef __WINDOWS__
    UNIX
#endif // __WINDOWS__
  };

  static Try<Address> create(const sockaddr_storage& storage)
  {
    switch (storage.ss_family) {
      case AF_INET:
#ifndef __WINDOWS__
      case AF_UNIX:
#endif // __WINDOWS__
        return Address(storage);
      default:
        return Error("Unsupported family: " + stringify(storage.ss_family));
    }
  }

  Family family() const
  {
    switch (sockaddr.storage.ss_family) {
      case AF_INET:
        return Family::INET;
#ifndef __WINDOWS__
      case AF_UNIX:
        return Family::UNIX;
#endif // __WINDOWS__
      default:
        ABORT("Unexpected family: " + stringify(sockaddr.storage.ss_family));
    }
  }

  // Returns the storage size depending on the family of this address.
  size_t size() const
  {
    switch (family()) {
      case Family::INET:
        return sizeof(sockaddr_in);
#ifndef __WINDOWS__
      case Family::UNIX:
        return sizeof(sockaddr_un);
#endif // __WINDOWS__
    }
    UNREACHABLE();
  }

  operator sockaddr_storage() const
  {
    return sockaddr.storage;
  }

private:
  friend class inet::Address;
#ifndef __WINDOWS__
  friend class unix::Address;
#endif // __WINDOWS__

  template <typename AddressType>
  friend Try<AddressType> convert(Try<Address>&& address);
  friend std::ostream& operator<<(std::ostream& stream, const Address& address);

  Address(const sockaddr_storage& storage)
  {
    sockaddr.storage = storage;
  }

  union {
    sockaddr_storage storage;
    sockaddr_in in;
#ifndef __WINDOWS__
    sockaddr_un un;
#endif // __WINDOWS__
  } sockaddr;
};


// Helper for converting between Address and other types.
template <typename AddressType>
Try<AddressType> convert(Try<Address>&& address);


template <>
inline Try<Address> convert(Try<Address>&& address)
{
  return address;
}


namespace inet {

class Address
{
public:
  Address(const net::IP& _ip, uint16_t _port)
    : ip(_ip), port(_port) {}

  Address(const sockaddr_in& in)
    : Address(net::IP(in.sin_addr), ntohs(in.sin_port)) {}

  static Address LOOPBACK_ANY() { return Address(net::IP(INADDR_LOOPBACK), 0); }

  static Address ANY_ANY() { return Address(net::IP(INADDR_ANY), 0); }

  /**
   * Returns the hostname of this address's IP.
   *
   * @returns the hostname of this address's IP.
   */
  // TODO(jmlvanre): Consider making this return a Future in order to
  // deal with slow name resolution.
  Try<std::string> hostname() const
  {
    const Try<std::string> hostname = ip == net::IP(INADDR_ANY)
      ? net::hostname()
      : net::getHostname(ip);

    if (hostname.isError()) {
      return Error(hostname.error());
    }

    return hostname.get();
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

  bool operator==(const network::Address& that) const;

  operator network::Address() const
  {
    union {
      sockaddr_storage storage;
      sockaddr_in in;
    } sockaddr;
    memset(&sockaddr.storage, 0, sizeof(sockaddr_storage));
    sockaddr.in.sin_family = AF_INET;
    sockaddr.in.sin_addr = ip.in().get();
    sockaddr.in.sin_port = htons(port);
    return network::Address(sockaddr.storage);
  }

  // TODO(benh): Use a sockaddr_in here like we do for unix::Address.
  net::IP ip;
  uint16_t port;
};


inline std::ostream& operator<<(std::ostream& stream, const Address& address)
{
  stream << address.ip << ":" << address.port;
  return stream;
}

} // namespace inet {


template <>
inline Try<inet::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  if (address->family() == Address::Family::INET) {
    return inet::Address(address->sockaddr.in);
  }

  return Error("Unexpected address family");
}


namespace inet {

inline bool Address::operator==(const network::Address& that) const
{
  Try<Address> address = convert<Address>(that);
  if (address.isError()) {
    return false;
  }
  return *this == address.get();
}

} // namespace inet {


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

    return Address(un);
  }

  Address(const sockaddr_un& un)
    : sockaddr() // Zero initialize.
  {
    sockaddr.un = un;
  }

  std::string path() const
  {
    if (sockaddr.un.sun_path[0] == '\0') {
      return '\0' + std::string(sockaddr.un.sun_path + 1);
    }

    return std::string(sockaddr.un.sun_path);
  }

  operator network::Address() const
  {
    return network::Address(sockaddr.storage);
  }

private:
  friend std::ostream& operator<<(
      std::ostream& stream,
      const Address& address);

  union {
    sockaddr_storage storage;
    sockaddr_un un;
  } sockaddr;
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


template <>
inline Try<unix::Address> convert(Try<Address>&& address)
{
  if (address.isError()) {
    return Error(address.error());
  }

  if (address->family() == Address::Family::UNIX) {
    return unix::Address(address->sockaddr.un);
  }

  return Error("Unexpected address family");
}
#endif // __WINDOWS__


inline std::ostream& operator<<(std::ostream& stream, const Address& address)
{
  switch (address.family()) {
    case Address::Family::INET:
      return stream << inet::Address(address.sockaddr.in);
#ifndef __WINDOWS__
    case Address::Family::UNIX:
      return stream << unix::Address(address.sockaddr.un);
#endif // __WINDOWS__
  }

  UNREACHABLE();
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

} // namespace std {

#endif // __PROCESS_ADDRESS_HPP__
