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


namespace net {

// Represents an IP address.
class IP
{
public:
  // Creates an IP from the given string that has the dot-decimal
  // format. For example:
  //   10.0.0.1
  //   192.168.1.100
  //   172.158.1.23
  static Try<IP> parse(const std::string& value);

  // Constructs an IP address from an unsigned integer (in host order).
  explicit IP(uint32_t _address) : address_(_address) {}

  // Returns the IP address (in host order).
  uint32_t address() const { return address_; }

  bool operator == (const IP& that) const
  {
    return address_ == that.address_;
  }

  bool operator != (const IP& that) const
  {
    return !operator == (that);
  }

private:
  // IP address (in host order).
  uint32_t address_;
};


inline Try<IP> IP::parse(const std::string& value)
{
  struct in_addr in;
  if (inet_aton(value.c_str(), &in) == 0) {
    return Error("Failed to parse the IP address");
  }

  return IP(ntohl(in.s_addr));
}


// Returns the string representation of the given IP address using the
// canonical dot-decimal form. For example: "10.0.0.1".
inline std::ostream& operator << (std::ostream& stream, const IP& ip)
{
  char buffer[INET_ADDRSTRLEN];

  struct in_addr addr;
  addr.s_addr = htonl(ip.address());

  const char* str = inet_ntop(AF_INET, &addr, buffer, sizeof(buffer));
  if (str == NULL) {
    // We do not expect inet_ntop to fail because all parameters
    // passed in are valid.
    const char *error_msg = strerror(errno);
    ABORT("inet_ntop returns error for address " + stringify(ip.address()) +
          ": " + error_msg);
  }

  stream << str;

  return stream;
}


// Represents an IP network composed from the IP address and the netmask
// which defines the subnet.
class IPNetwork
{
public:
  // Creates an IP network from the given string that has the dot-decimal
  // format with subnet prefix). For example:
  //   10.0.0.1/8
  //   192.168.1.100/24
  //   172.158.1.23
  static Try<IPNetwork> parse(const std::string& value);

  // Creates an IP network from the given IP address
  // and the given netmask.
  // Returns error if the netmask is not valid (e.g., not contiguous).
  static Try<IPNetwork> fromAddressNetmask(
      const IP& address,
      const IP& netmask);

  // Creates an IP network from an IP address and a subnet
  // prefix (within [0,32]). Returns error if the prefix is not valid.
  static Try<IPNetwork> fromAddressPrefix(const IP& address, int prefix);

  // Returns the IP address.
  IP address() const { return address_; }

  // Returns the netmask.
  IP netmask() const { return netmask_; }

  // Returns the prefix of the subnet defined by the netmask.
  int prefix() const
  {
    uint32_t mask = netmask_.address();

    int value = 0;
    while (mask != 0) {
      value += mask & 1;
      mask >>= 1;
    }

    return value;
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
  // Constructs an IP with the given IP address and the given netmask.
  IPNetwork(const IP& _address, const IP& _netmask)
    : address_(_address), netmask_(_netmask) {}

  // IP address.
  IP address_;

  // Netmask.
  IP netmask_;
};


inline Try<IPNetwork> IPNetwork::parse(const std::string& value)
{
  std::vector<std::string> tokens = strings::split(value, "/");

  if (tokens.size() != 2) {
    return Error("Unexpected number of '/' detected: " + string(tokens.size()));
  }

  // Parse the IP address.
  Try<IP> ip = IP::parse(tokens[0]);
  if (ip.isError()) {
    return Error(ip.error());
  }

  // Parse the subnet prefix.
  Try<int> prefix = numify<int>(tokens[1]);
  if (prefix.isError()) {
    return Error("Subnet prefix is not a number");
  }

  return fromAddressPrefix(ip.get(), prefix.get());
}


inline Try<IPNetwork> IPNetwork::fromAddressNetmask(
    const IP& address,
    const IP& netmask)
{
  uint32_t mask = netmask.address();
  if (((~mask + 1) & (~mask)) != 0) {
    return Error("Netmask is not valid");
  }

  return IPNetwork(address, netmask);
}


inline Try<IPNetwork> IPNetwork::fromAddressPrefix(
    const IP& address,
    int prefix)
{
  if (prefix > 32) {
    return Error("Subnet prefix is larger than 32");
  }

  if (prefix < 0) {
    return Error("Subnet prefix is negative");
  }

  return IPNetwork(address, IP(0xffffffff << (32 - prefix)));
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


// Returns the first available IP network of a given link device.
// The link device is specified using its name (e.g., eth0).
// Returns error if the link device is not found.
// Returns none if the link device is found,
// but does not have an IP network.
// TODO(jieyu): It is uncommon, but likely that a link device has
// multiple IP networks. In that case, consider returning
// the primary IP network instead of the first one.
inline Result<IPNetwork> fromLinkDevice(const std::string& name)
{
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

      if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET) {
        struct sockaddr_in* addr = (struct sockaddr_in*) ifa->ifa_addr;

        if (ifa->ifa_netmask != NULL &&
            ifa->ifa_netmask->sa_family == AF_INET) {
          struct sockaddr_in* netmask = (struct sockaddr_in*) ifa->ifa_netmask;

          Try<IPNetwork> network = IPNetwork::fromAddressNetmask(
              IP(ntohl(addr->sin_addr.s_addr)),
              IP(ntohl(netmask->sin_addr.s_addr)));

          freeifaddrs(ifaddr);

          if (network.isError()) {
            return Error(network.error());
          }

          return network.get();
        }

        // Note that this is the case where netmask is not specified.
        // A default /32 value is used.
        // We've seen such cases when VPN is used.
        Try<IPNetwork> ip = IPNetwork::fromAddressPrefix(
            IP(ntohl(addr->sin_addr.s_addr)),
            32);

        freeifaddrs(ifaddr);
        if (ip.isError()) {
          return Error(ip.error());
        }

        return ip.get();
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


} // namespace net {

#endif // __STOUT_IP_HPP__
