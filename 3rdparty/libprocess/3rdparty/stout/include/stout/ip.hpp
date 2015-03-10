/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_IP_HPP__
#define __STOUT_IP_HPP__

#if defined(__linux__) || defined(__APPLE__)
#include <ifaddrs.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif

#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <iostream>
#include <string>

#include "abort.hpp"
#include "error.hpp"
#include "none.hpp"
#include "numify.hpp"
#include "option.hpp"
#include "result.hpp"
#include "stringify.hpp"
#include "strings.hpp"
#include "try.hpp"
#include "unreachable.hpp"


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
  static Try<IP> parse(const std::string& value, int family);

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

  // Creates an IP from a 32 bit unsigned integer. Note that the
  // integer stores the IP address in host order.
  explicit IP(uint32_t _storage)
    : family_(AF_INET)
  {
     clear();
     storage_.in_.s_addr = htonl(_storage);
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
      return Error("Unsupported family type: " + stringify(family_));
    }
  }

  // Checks if this IP is for loopback (e.g., INADDR_LOOPBACK).
  bool isLoopback() const
  {
    switch (family_) {
      case AF_INET:
        return storage_.in_.s_addr == htonl(INADDR_LOOPBACK);
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
      default:
        UNREACHABLE();
    }
  }

  bool operator == (const IP& that) const
  {
    if (family_ != that.family_) {
      return false;
    } else {
      return memcmp(&storage_, &that.storage_, sizeof(storage_)) == 0;
    }
  }

  bool operator != (const IP& that) const
  {
    return !operator == (that);
  }

  bool operator < (const IP& that) const
  {
    if (family_ != that.family_) {
      return family_ < that.family_;
    } else {
      return memcmp(&storage_, &that.storage_, sizeof(storage_)) < 0;
    }
  }

  bool operator > (const IP& that) const
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
  };

  int family_;
  Storage storage_;
};


inline Try<IP> IP::parse(const std::string& value, int family)
{
  Storage storage;
  switch (family) {
    case AF_INET: {
      if (inet_pton(AF_INET, value.c_str(), &storage.in_) == 0) {
        return Error("Failed to parse the IP");
      }
      return IP(storage.in_);
    }
    default: {
      return Error("Unsupported family type: " + stringify(family));
    }
  }
}


inline Try<IP> IP::create(const struct sockaddr_storage& _storage)
{
  switch (_storage.ss_family) {
    case AF_INET: {
      struct sockaddr_in addr = *((struct sockaddr_in*) &_storage);
      return IP(addr.sin_addr);
    }
    default: {
      return Error(
          "Unsupported family type: " +
          stringify(_storage.ss_family));
    }
  }
}


inline Try<IP> IP::create(const struct sockaddr& _storage)
{
  return create(*((struct sockaddr_storage*) &_storage));
}


// Returns the string representation of the given IP using the
// canonical dot-decimal form. For example: "10.0.0.1".
inline std::ostream& operator << (std::ostream& stream, const IP& ip)
{
  switch (ip.family()) {
    case AF_INET: {
      char buffer[INET_ADDRSTRLEN];
      struct in_addr in = ip.in().get();
      if (inet_ntop(AF_INET, &in, buffer, sizeof(buffer)) == NULL) {
        // We do not expect inet_ntop to fail because all parameters
        // passed in are valid.
        ABORT("Failed to get human-readable IP for " +
              stringify(ntohl(in.s_addr)) +
              ": " + strerror(errno));
      }

      stream << buffer;
      return stream;
    }
    default: {
      UNREACHABLE();
    }
  }
}


inline std::size_t hash_value(const IP& ip)
{
  size_t seed = 0;

  switch (ip.family()) {
     case AF_INET:
       boost::hash_combine(seed, htonl(ip.in().get().s_addr));
       return seed;
     default:
       UNREACHABLE();
  }
}


// Represents an IP network. We store the IP address and the IP
// netmask which defines the subnet.
class IPNetwork
{
public:
  // Returns the IPv4 network for loopback (i.e., 127.0.0.1/8).
  static IPNetwork LOOPBACK_V4();

  // Creates an IP network from the given string that has the
  // dot-decimal format with subnet prefix).
  // For example:
  //   10.0.0.1/8
  //   192.168.1.100/24
  //   172.158.1.23
  static Try<IPNetwork> parse(const std::string& value, int family);

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
        uint32_t mask = ntohl(netmask_.in().get().s_addr);
        int value = 0;

        while (mask != 0) {
          value += mask & 1;
          mask >>= 1;
        }

        return value;
      }
      default: {
        UNREACHABLE();
      }
    }
  }

  bool operator == (const IPNetwork& that) const
  {
    return address_ == that.address_ && netmask_ == that.netmask_;
  }

  bool operator != (const IPNetwork& that) const
  {
    return !operator == (that);
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
        return Error("Netmask is not valid");
      }

      return IPNetwork(address, netmask);
    }
    default: {
      UNREACHABLE();
    }
  }
}


inline Try<IPNetwork> IPNetwork::create(const IP& address, int prefix)
{
  if (prefix < 0) {
    return Error("Subnet prefix is negative");
  }

  switch (address.family()) {
    case AF_INET: {
      if (prefix > 32) {
        return Error("Subnet prefix is larger than 32");
      }

      return IPNetwork(address, IP(0xffffffff << (32 - prefix)));
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
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  if (family != AF_INET) {
    return Error("Unsupported family type: " + stringify(family));
  }

  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

      if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == family) {
        IP address = IP::create(*ifa->ifa_addr).get();

        if (ifa->ifa_netmask != NULL &&
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
        // default /32 prefix is used.
        Try<IPNetwork> network = IPNetwork::create(address, 32);
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
// canonical dot-decimal form with prefix. For example: "10.0.0.1/8".
inline std::ostream& operator << (
    std::ostream& stream,
    const IPNetwork& network)
{
  stream << network.address() << "/" << network.prefix();

  return stream;
}

} // namespace net {

#endif // __STOUT_IP_HPP__
