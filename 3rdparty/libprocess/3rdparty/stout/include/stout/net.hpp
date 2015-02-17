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
#ifndef __STOUT_NET_HPP__
#define __STOUT_NET_HPP__

#if defined(__linux__) || defined(__APPLE__)
#include <ifaddrs.h>
#endif
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>

#ifdef __linux__
#include <linux/if.h>
#include <linux/if_packet.h>
#endif

#include <net/ethernet.h>

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <curl/curl.h>

#include <iostream>
#include <set>
#include <string>

#include "abort.hpp"
#include "error.hpp"
#include "none.hpp"
#include "option.hpp"
#include "os.hpp"
#include "result.hpp"
#include "stringify.hpp"
#include "strings.hpp"
#include "try.hpp"


// Network utilities.
namespace net {

inline struct addrinfo createAddrInfo(int socktype, int family, int flags)
{
  struct addrinfo addr;
  memset(&addr, 0, sizeof(addr));
  addr.ai_socktype = socktype;
  addr.ai_family = family;
  addr.ai_flags |= flags;

  return addr;
}


// TODO(evelinad): Add createSockaddrIn6 when will support IPv6
inline struct sockaddr_in createSockaddrIn(uint32_t ip, int port)
{
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ip;
  addr.sin_port = htons(port);

  return addr;
}


// Returns the HTTP response code resulting from attempting to
// download the specified HTTP or FTP URL into a file at the specified
// path.
inline Try<int> download(const std::string& url, const std::string& path)
{
  Try<int> fd = os::open(
      path,
      O_CREAT | O_WRONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error(fd.error());
  }

  curl_global_init(CURL_GLOBAL_ALL);
  CURL* curl = curl_easy_init();

  if (curl == NULL) {
    curl_easy_cleanup(curl);
    os::close(fd.get());
    return Error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

  FILE* file = fdopen(fd.get(), "w");
  if (file == NULL) {
    return ErrnoError("Failed to open file handle of '" + path + "'");
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    fclose(file);
    return Error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (fclose(file) != 0) {
    return ErrnoError("Failed to close file handle of '" + path + "'");
  }

  return Try<int>::some(code);
}


inline Try<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return ErrnoError();
  }

  // TODO(evelinad): Add AF_UNSPEC when we will support IPv6
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, AF_INET, AI_CANONNAME);
  struct addrinfo *result;

  int error = getaddrinfo(host, NULL, &hints, &result);

  if (error != 0 || result == NULL) {
    if (result != NULL) {
      freeaddrinfo(result);
    }
    return Error(gai_strerror(error));
  }

  std::string hostname = result->ai_canonname;
  freeaddrinfo(result);

  return hostname;
}


// Returns a Try of the hostname for the provided IP. If the hostname
// cannot be resolved, then a string version of the IP address is
// returned.
inline Try<std::string> getHostname(uint32_t ip)
{
  sockaddr_in addr = createSockaddrIn(ip, 0);

  char hostname[MAXHOSTNAMELEN];
  int error = getnameinfo(
      (sockaddr*)&addr,
      sizeof(addr),
      hostname,
      MAXHOSTNAMELEN,
      NULL,
      0,
      0);
  if (error != 0) {
    return Error(std::string(gai_strerror(error)));
  }

  return std::string(hostname);
}


// Returns a Try of the IP for the provided hostname or an error if no
// IP is obtained.
inline Try<uint32_t> getIP(const std::string& hostname, sa_family_t family)
{
  struct addrinfo hints, *result;
  hints = createAddrInfo(SOCK_STREAM, family, 0);

  int error = getaddrinfo(hostname.c_str(), NULL, &hints, &result);
  if (error != 0 || result == NULL) {
    if (result != NULL ) {
      freeaddrinfo(result);
    }
    return Error(gai_strerror(error));
  }
  if (result->ai_addr == NULL) {
    freeaddrinfo(result);
    return Error("Got no addresses for '" + hostname + "'");
  }

  uint32_t ip = ((struct sockaddr_in*)(result->ai_addr))->sin_addr.s_addr;
  freeaddrinfo(result);

  return ip;
}


// Returns the names of all the link devices in the system.
inline Try<std::set<std::string> > links()
{
#if !defined(__linux__) && !defined(__APPLE__)
  return Error("Not implemented");
#else
  struct ifaddrs* ifaddr = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  std::set<std::string> names;
  for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != NULL) {
      names.insert(ifa->ifa_name);
    }
  }

  freeifaddrs(ifaddr);
  return names;
#endif
}


// Represents a MAC address. A MAC address is a 48-bit unique
// identifier assigned to a network interface for communications on
// the physical network segment. We use a byte array (in transmission
// order) to represent a MAC address. For example, for a MAC address
// 01:23:34:67:89:ab, the format is shown as follows:
//
//    MSB                                          LSB
//     |                                            |
//     v                                            v
// +--------+--------+--------+--------+--------+--------+
// |bytes[0]|bytes[1]|bytes[2]|bytes[3]|bytes[4]|bytes[5]|
// +--------+--------+--------+--------+--------+--------+
//
//     01   :   23   :   45   :   67   :   89   :   ab
// NOLINT(readability/ending_punctuation)
class MAC
{
public:
  // Constructs a MAC address from a byte array.
  explicit MAC(const uint8_t (&_bytes)[6])
  {
    for (size_t i = 0; i < 6; i++) {
      bytes[i] = _bytes[i];
    }
  }

  // Returns the byte at the given index. For example, for a MAC
  // address 01:23:45:67:89:ab, mac[0] = 01, mac[1] = 23 and etc.
  uint8_t operator [] (size_t index) const
  {
    if (index >= 6) {
      ABORT("Invalid index specified in MAC::operator []\n");
    }

    return bytes[index];
  }

  bool operator == (const MAC& that) const
  {
    for (size_t i = 0; i < 6; i++) {
      if (bytes[i] != that.bytes[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator != (const MAC& that) const
  {
    return !operator == (that);
  }

private:
  // Byte array of this MAC address (in transmission order).
  uint8_t bytes[6];
};


// Returns the standard string format (IEEE 802) of the given MAC
// address, which contains six groups of two hexadecimal digits,
// separated by colons, in transmission order (e.g.,
// 01:23:45:67:89:ab).
inline std::ostream& operator << (std::ostream& stream, const MAC& mac)
{
  char buffer[18];

  sprintf(
      buffer,
      "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
      mac[0],
      mac[1],
      mac[2],
      mac[3],
      mac[4],
      mac[5]);

  return stream << buffer;
}


// Returns the MAC address of a given link device. The link device is
// specified using its name (e.g., eth0). Returns error if the link
// device is not found. Returns none if the link device is found, but
// does not have a MAC address (e.g., loopback).
inline Result<MAC> mac(const std::string& name)
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

# if defined(__linux__)
     if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_PACKET) {
        struct sockaddr_ll* link = (struct sockaddr_ll*) ifa->ifa_addr;

        if (link->sll_halen == 6) {
          struct ether_addr* addr = (struct ether_addr*) link->sll_addr;
          MAC mac(addr->ether_addr_octet);

          // Ignore if the address is 0 so that the results are
          // consistent on both OSX and Linux.
          if (stringify(mac) == "00:00:00:00:00:00") {
            continue;
          }

          freeifaddrs(ifaddr);
          return mac;
        }
      }
# elif defined(__APPLE__)
      if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_LINK) {
        struct sockaddr_dl* link = (struct sockaddr_dl*) ifa->ifa_addr;

        if (link->sdl_type == IFT_ETHER && link->sdl_alen == 6) {
          struct ether_addr* addr = (struct ether_addr*) LLADDR(link);
          MAC mac(addr->octet);

          freeifaddrs(ifaddr);
          return mac;
        }
      }
# endif
    }
  }

  freeifaddrs(ifaddr);

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
#endif
}


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
inline Result<IP> ip(const std::string& name)
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

#endif // __STOUT_NET_HPP__
