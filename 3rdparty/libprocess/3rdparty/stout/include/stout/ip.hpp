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


// Network utilities.
namespace net {

// Represents an IPv4 address. Besides the actual IP address, we also
// store additional information about the address such as the netmask
// which defines the subnet.
class IP
{
public:
  // Creates an IP from the given string that has the dot-decimal
  // format (with or without subnet prefix). For example:
  //   10.0.0.1/8
  //   192.168.1.100/24
  //   172.158.1.23
  static Try<IP> fromDotDecimal(const std::string& value);

  // Creates an IP from the given IP address (in host order) and the
  // given netmask (in host order). Returns error if the netmask is
  // not valid (e.g., not contiguous).
  static Try<IP> fromAddressNetmask(uint32_t address, uint32_t netmask);

  // Creates an IP from a 32-bit address (in host order) and a subnet
  // prefix (within [0,32]). Returns error if the prefix is not valid.
  static Try<IP> fromAddressPrefix(uint32_t address, size_t prefix);

  // Constructs an IP with the given IP address (in host order).
  explicit IP(uint32_t _address) : address_(_address) {}

  // Returns the IP address (in host order).
  uint32_t address() const { return address_; }

  // Returns the net mask (in host order).
  Option<uint32_t> netmask() const { return netmask_; }

  // Returns the prefix of the subnet defined by the net mask.
  Option<size_t> prefix() const
  {
    if (netmask_.isNone()) {
      return None();
    }

    uint32_t mask = netmask_.get();

    size_t value = 0;
    while (mask != 0) {
      value += mask & 1;
      mask >>= 1;
    }

    return value;
  }

  bool operator == (const IP& that) const
  {
    return address_ == that.address_ && netmask_ == that.netmask_;
  }

  bool operator != (const IP& that) const
  {
    return !operator == (that);
  }

private:
  // Constructs an IP with the given IP address (in host order) and
  // the given netmask (in host order).
  IP(uint32_t _address, uint32_t _netmask)
    : address_(_address), netmask_(_netmask) {}

  // IP address (in host order).
  uint32_t address_;

  // The optional net mask (in host order).
  Option<uint32_t> netmask_;
};


inline Try<IP> IP::fromDotDecimal(const std::string& value)
{
  std::vector<std::string> tokens = strings::split(value, "/");

  if (tokens.size() > 2) {
    return Error("More than one '/' detected");
  }

  struct in_addr in;
  if (inet_aton(tokens[0].c_str(), &in) == 0) {
    return Error("Failed to parse the IP address");
  }

  // Parse the subnet prefix if specified.
  if (tokens.size() == 2) {
    Try<size_t> prefix = numify<size_t>(tokens[1]);
    if (prefix.isError()) {
      return Error("Subnet prefix is not a number");
    }

    return fromAddressPrefix(ntohl(in.s_addr), prefix.get());
  }

  return IP(ntohl(in.s_addr));
}


inline Try<IP> IP::fromAddressNetmask(uint32_t address, uint32_t netmask)
{
  if (((~netmask + 1) & (~netmask)) != 0) {
    return Error("Netmask is not valid");
  }

  return IP(address, netmask);
}


inline Try<IP> IP::fromAddressPrefix(uint32_t address, size_t prefix)
{
  if (prefix > 32) {
    return Error("Subnet prefix is larger than 32");
  }

  return IP(address, (0xffffffff << (32 - prefix)));
}


// Returns the string representation of the given IP address using the
// canonical dot-decimal form with prefix. For example: "10.0.0.1/8".
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

  if (ip.prefix().isSome()) {
    stream << "/" << ip.prefix().get();
  }

  return stream;
}


// Returns the first available IPv4 address of a given link device.
// The link device is specified using its name (e.g., eth0). Returns
// error if the link device is not found. Returns none if the link
// device is found, but does not have an IPv4 address.
// TODO(jieyu): It is uncommon, but likely that a link device has
// multiple IPv4 addresses. In that case, consider returning the
// primary IP address instead of the first one.
inline Result<IP> fromLinkDevice(const std::string& name)
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

          Try<IP> ip = IP::fromAddressNetmask(
              ntohl(addr->sin_addr.s_addr),
              ntohl(netmask->sin_addr.s_addr));

          freeifaddrs(ifaddr);

          if (ip.isError()) {
            return Error(ip.error());
          }

          return ip.get();
        }

        // Note that this is the case where net mask is not specified.
        // We've seen such cases when VPN is used.
        IP ip(ntohl(addr->sin_addr.s_addr));

        freeifaddrs(ifaddr);
        return ip;
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
