// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_IP_HPP__
#define __STOUT_IP_HPP__

// For 'sockaddr'.
#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <ifaddrs.h>
#endif // __linux__ || __APPLE__ || __FreeBSD__
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif // __APPLE__

// Note: Header grouping and ordering is considered before
// inclusion/exclusion by platform.
// For 'inet_pton', 'inet_ntop'.
#ifdef __WINDOWS__
#include <Ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif // __WINDOWS__

#include <sys/types.h>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>

#include <stout/abort.hpp>
#include <stout/bits.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os/strerror.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__


namespace net {

// Represents an IP.
class IP
{
public:
  // Creates an IP from the given string that has the dot-decimal
  // format. For example:
  //   10.0.0.1
  //   192.168.1.100
  //   172.158.1.23
  //   2001:db8:85a3::8a2e:370:7334
  static Try<IP> parse(const std::string& value, int family = AF_UNSPEC);

  // Creates an IP from a struct sockaddr_storage.
  static Try<IP> create(const struct sockaddr_storage& _storage);

  // Creates an IP from a struct sockaddr.
  static Try<IP> create(const struct sockaddr& _storage);

  // Creates an IP from struct in_addr. Note that by standard, struct
  // in_addr stores the IP address in network order.
  explicit IP(const struct in_addr& _storage)
    : family_(AF_INET)
  {
     clear();
     storage_.in_ = _storage;
  }

  // Creates an IP from struct in6_addr. Note that by standard, struct
  // in_addr stores the IP address in network order.
  explicit IP(const struct in6_addr& _storage)
    : family_(AF_INET6)
  {
     clear();
     storage_.in6_ = _storage;
  }

  // Creates an IP from a 32 bit unsigned integer. Note that the
  // integer stores the IP address in host order.
  explicit IP(uint32_t _ip)
    : family_(AF_INET)
  {
     clear();
     storage_.in_.s_addr = htonl(_ip);
  }

  // Returns the family type.
  int family() const
  {
    return family_;
  }

  // Returns the struct in_addr storage.
  Try<struct in_addr> in() const
  {
    if (family_ == AF_INET) {
      return storage_.in_;
    } else {
      return Error("Cannot create in_addr from family: " + stringify(family_));
    }
  }

  // Returns the struct in6_addr storage.
  Try<struct in6_addr> in6() const
  {
    if (family_ == AF_INET6) {
      return storage_.in6_;
    } else {
      return Error("Cannot create in6_addr from family: " + stringify(family_));
    }
  }

  // Checks if this IP is for loopback (e.g., INADDR_LOOPBACK).
  bool isLoopback() const
  {
    switch (family_) {
      case AF_INET:
        return storage_.in_.s_addr == htonl(INADDR_LOOPBACK);
      case AF_INET6:
        return !memcmp(&storage_.in6_.s6_addr, &in6addr_loopback.s6_addr, 16);
      default:
        UNREACHABLE();
    }
  }

  // Checks if this IP is for any incoming address (e.g., INADDR_ANY).
  bool isAny() const
  {
    switch (family_) {
      case AF_INET:
        return storage_.in_.s_addr == htonl(INADDR_ANY);
      case AF_INET6:
        return !memcmp(&storage_.in6_.s6_addr, &in6addr_any.s6_addr, 16);
      default:
        UNREACHABLE();
    }
  }

  bool operator==(const IP& that) const
  {
    if (family_ != that.family_) {
      return false;
    } else {
      return memcmp(&storage_, &that.storage_, sizeof(storage_)) == 0;
    }
  }

  bool operator!=(const IP& that) const
  {
    return !(*this == that);
  }

  bool operator<(const IP& that) const
  {
    if (family_ != that.family_) {
      return family_ < that.family_;
    } else {
      return memcmp(&storage_, &that.storage_, sizeof(storage_)) < 0;
    }
  }

  bool operator>(const IP& that) const
  {
    if (family_ != that.family_) {
      return family_ > that.family_;
    } else {
      return memcmp(&storage_, &that.storage_, sizeof(storage_)) > 0;
    }
  }

private:
  // NOTE: We need to clear the union when creating an IP because the
  // equality check uses memcmp.
  void clear()
  {
    memset(&storage_, 0, sizeof(storage_));
  }

  union Storage
  {
    struct in_addr in_;
    struct in6_addr in6_;
  };

  int family_;
  Storage storage_;
};


inline Try<IP> IP::parse(const std::string& value, int family)
{
  Storage storage;
  switch (family) {
  case AF_INET: {
    if (inet_pton(AF_INET, value.c_str(), &storage.in_) == 1) {
      return IP(storage.in_);
    }

    return Error("Failed to parse IPv4: " + value);
  }
  case AF_INET6: {
    if (inet_pton(AF_INET6, value.c_str(), &storage.in6_) == 1) {
      return IP(storage.in6_);
    }

    return Error("Failed to parse IPv6: " + value);
  }
  case AF_UNSPEC: {
    Try<IP> ip4 = parse(value, AF_INET);
    if (ip4.isSome()) {
      return ip4;
    }

    Try<IP> ip6 = parse(value, AF_INET6);
    if (ip6.isSome()) {
      return ip6;
    }

    return Error("Failed to parse IP as either IPv4 or IPv6:" + value);
  }
  default:
    return Error("Unsupported family type: " + stringify(family));
  }
}


inline Try<IP> IP::create(const struct sockaddr_storage& _storage)
{
  // According to POSIX: (IEEE Std 1003.1, 2004)
  //
  // (1) `sockaddr_storage` is "aligned at an appropriate boundary so that
  // pointers to it can be cast as pointers to protocol-specific address
  // structures and used to access the fields of those structures without
  // alignment problems."
  //
  // (2) "When a `sockaddr_storage` structure is cast as a `sockaddr`
  // structure, the `ss_family` field of the `sockaddr_storage` structure
  // shall map onto the `sa_family` field of the `sockaddr` structure."
  //
  // Therefore, casting from `const sockaddr_storage*` to `const sockaddr*`
  // (then subsequently dereferencing the `const sockaddr*`) should be safe.
  // Note that casting in the reverse direction (`const sockaddr*` to
  // `const sockaddr_storage*`) would NOT be safe, since the former might
  // not be aligned appropriately.
  const struct sockaddr* addr =
    reinterpret_cast<const struct sockaddr*>(&_storage);

  return create(*addr);
}


inline Try<IP> IP::create(const struct sockaddr& addr)
{
  switch (addr.sa_family) {
    case AF_INET: {
      const struct sockaddr_in& addr4 =
        reinterpret_cast<const struct sockaddr_in&>(addr);

      return IP(addr4.sin_addr);
    }
    case AF_INET6: {
      const struct sockaddr_in6& addr6 =
        reinterpret_cast<const struct sockaddr_in6&>(addr);

      return IP(addr6.sin6_addr);
    }
    default: {
      return Error("Unsupported family type: " + stringify(addr.sa_family));
    }
  }
}


// Returns the string representation of the given IP using the
// canonical form, for example: "10.0.0.1" or "fe80::1".
inline std::ostream& operator<<(std::ostream& stream, const IP& ip)
{
  switch (ip.family()) {
    case AF_INET: {
      char buffer[INET_ADDRSTRLEN];
      struct in_addr in = ip.in().get();
      if (inet_ntop(AF_INET, &in, buffer, sizeof(buffer)) == nullptr) {
        // We do not expect inet_ntop to fail because all parameters
        // passed in are valid.
        ABORT("Failed to get human-readable IPv4 for " +
              stringify(ntohl(in.s_addr)) + ": " + os::strerror(errno));
      }
      return stream << buffer;
    }
    case AF_INET6: {
      char buffer[INET6_ADDRSTRLEN];
      struct in6_addr in6 = ip.in6().get();
      if (inet_ntop(AF_INET6, &in6, buffer, sizeof(buffer)) == nullptr) {
        ABORT("Failed to get human-readable IPv6: " + os::strerror(errno));
      }
      return stream << buffer;
    }
    default: {
      UNREACHABLE();
    }
  }
}


// Represents an IP network. We store the IP address and the IP
// netmask which defines the subnet.
class IPNetwork
{
public:
  // Returns the IPv4 network for loopback (i.e., 127.0.0.1/8).
  static IPNetwork LOOPBACK_V4();

  // Returns the IPv6 network for loopback (i.e. ::1/128)
  static IPNetwork LOOPBACK_V6();

  // Creates an IP network from the given string that has the
  // IP address in canonical format with subnet prefix.
  // For example:
  //   10.0.0.1/8
  //   192.168.1.100/24
  //   fe80::3/64
  static Try<IPNetwork> parse(const std::string& value, int family = AF_UNSPEC);

  // Creates an IP network from the given IP address and netmask.
  // Returns error if the netmask is not valid (e.g., not contiguous).
  static Try<IPNetwork> create(const IP& address, const IP& netmask);

  // Creates an IP network from an IP address and a subnet prefix.
  // Returns error if the prefix is not valid.
  static Try<IPNetwork> create(const IP& address, int prefix);

  // Returns the first available IP network of a given link device.
  // The link device is specified using its name (e.g., eth0). Returns
  // error if the link device is not found. Returns none if the link
  // device is found, but does not have an IP network.
  // TODO(jieyu): It is uncommon, but likely that a link device has
  // multiple IP networks. In that case, consider returning the
  // primary IP network instead of the first one.
  static Result<IPNetwork> fromLinkDevice(const std::string& name, int family);

  IP address() const { return address_; }

  IP netmask() const { return netmask_; }

  // Returns the prefix of the subnet defined by the IP netmask.
  int prefix() const
  {
    switch (netmask_.family()) {
      case AF_INET: {
        return bits::countSetBits(netmask_.in().get().s_addr);
      }
      case AF_INET6: {
        struct in6_addr in6 = netmask_.in6().get();

        int prefix = std::accumulate(
          std::begin(in6.s6_addr),
          std::end(in6.s6_addr),
          0,
          [](int acc, uint8_t c) { return acc + bits::countSetBits(c); });

        return prefix;
      }
      default: {
        UNREACHABLE();
      }
    }
  }

  bool operator==(const IPNetwork& that) const
  {
    return address_ == that.address_ && netmask_ == that.netmask_;
  }

  bool operator!=(const IPNetwork& that) const
  {
    return !(*this == that);
  }

private:
  IPNetwork(const IP& _address, const IP& _netmask)
    : address_(_address), netmask_(_netmask) {}

  IP address_;
  IP netmask_;
};


inline Try<IPNetwork> IPNetwork::parse(const std::string& value, int family)
{
  std::vector<std::string> tokens = strings::split(value, "/");

  if (tokens.size() != 2) {
    return Error(
        "Unexpected number of '/' detected: " +
        stringify(tokens.size()));
  }

  // Parse the IP address.
  Try<IP> address = IP::parse(tokens[0], family);
  if (address.isError()) {
    return Error("Failed to parse the IP address: " + address.error());
  }

  // Parse the subnet prefix.
  Try<int> prefix = numify<int>(tokens[1]);
  if (prefix.isError()) {
    return Error("Subnet prefix is not a number");
  }

  return create(address.get(), prefix.get());
}


inline IPNetwork IPNetwork::LOOPBACK_V4()
{
  return parse("127.0.0.1/8", AF_INET).get();
}


inline IPNetwork IPNetwork::LOOPBACK_V6()
{
  return parse("::1/128", AF_INET6).get();
}


inline Try<IPNetwork> IPNetwork::create(const IP& address, const IP& netmask)
{
  if (address.family() != netmask.family()) {
    return Error(
        "The network families of the IP address '" +
        stringify(address.family()) + "' and the IP netmask '" +
        stringify(netmask.family()) + "' do not match");
  }

  switch (address.family()) {
    case AF_INET: {
      uint32_t mask = ntohl(netmask.in().get().s_addr);
      if (((~mask + 1) & (~mask)) != 0) {
        return Error("IPv4 netmask is not valid");
      }
      break;
    }
    case AF_INET6: {
      in6_addr mask = netmask.in6().get();

      uint8_t testMask = 0xff;
      for (int i = 0; i < 16; i++) {
        if (mask.s6_addr[i] != testMask) {
          if (testMask == 0) {
            return Error("IPv6 netmask is not valid");
          }

          if (((uint8_t)(~mask.s6_addr[i] + 1) & (~mask.s6_addr[i])) != 0) {
            return Error("IPv6 netmask is not valid");
          }

          testMask = 0;
        }
      }
      break;
    }
    default: {
      UNREACHABLE();
    }
  }

  return IPNetwork(address, netmask);
}


inline Try<IPNetwork> IPNetwork::create(const IP& address, int prefix)
{
  if (prefix < 0) {
    return Error("Subnet prefix is negative");
  }

  switch (address.family()) {
    case AF_INET: {
      if (prefix > 32) {
        return Error("IPv4 subnet prefix is larger than 32");
      }

      // Avoid left-shifting by 32 bits when prefix is 0.
      uint32_t mask = 0;
      if (prefix > 0) {
        mask = 0xffffffff << (32 - prefix);
      }

      return IPNetwork(address, IP(mask));
    }
    case AF_INET6: {
      if (prefix > 128) {
        return Error("IPv6 subnet prefix is larger than 128");
      }

      in6_addr mask;
      memset(&mask, 0, sizeof(mask));

      int i = 0;
      while (prefix >= 8) {
        mask.s6_addr[i++] = 0xff;
        prefix -= 8;
      }

      if (prefix > 0) {
        uint8_t _mask = 0xff << (8 - prefix);
        mask.s6_addr[i] = _mask;
      }

      return IPNetwork(address, IP(mask));
    }
    default: {
      UNREACHABLE();
    }
  }
}


inline Result<IPNetwork> IPNetwork::fromLinkDevice(
    const std::string& name,
    int family)
{
#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__)
  return Error("Not implemented");
#else
  if (family != AF_INET && family != AF_INET6) {
    return Error("Unsupported family type: " + stringify(family));
  }

  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != nullptr && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

      if (ifa->ifa_addr != nullptr && ifa->ifa_addr->sa_family == family) {
        IP address = IP::create(*ifa->ifa_addr).get();

        if (ifa->ifa_netmask != nullptr &&
            ifa->ifa_netmask->sa_family == family) {
          IP netmask = IP::create(*ifa->ifa_netmask).get();

          freeifaddrs(ifaddr);

          Try<IPNetwork> network = IPNetwork::create(address, netmask);
          if (network.isError()) {
            return Error(network.error());
          }

          return network.get();
        }

        freeifaddrs(ifaddr);

        // Note that this is the case where netmask is not specified.
        // We've seen such cases when VPN is used. In that case, a
        // default /32 prefix for IPv4 and /64 for IPv6 is used.
        int prefix = (family == AF_INET ? 32 : 64);

        Try<IPNetwork> network = IPNetwork::create(address, prefix);
        if (network.isError()) {
          return Error(network.error());
        }

        return network.get();
      }
    }
  }

  freeifaddrs(ifaddr);

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
#endif
}


// Returns the string representation of the given IP network using the
// canonical form with prefix. For example: "10.0.0.1/8".
inline std::ostream& operator<<(std::ostream& stream, const IPNetwork& network)
{
  stream << network.address() << "/" << network.prefix();

  return stream;
}

} // namespace net {

namespace std {

template <>
struct hash<net::IP>
{
  typedef size_t result_type;

  typedef net::IP argument_type;

  result_type operator()(const argument_type& ip) const
  {
    size_t seed = 0;

    switch (ip.family()) {
      case AF_INET:
        boost::hash_combine(seed, htonl(ip.in().get().s_addr));
        return seed;
      case AF_INET6: {
        in6_addr in6 = ip.in6().get();
        boost::hash_range(seed, std::begin(in6.s6_addr), std::end(in6.s6_addr));
        return seed;
      }
      default:
        UNREACHABLE();
    }
  }
};

} // namespace std {

#endif // __STOUT_IP_HPP__
