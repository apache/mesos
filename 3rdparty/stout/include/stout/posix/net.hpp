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

#ifndef __STOUT_POSIX_NET_HPP__
#define __STOUT_POSIX_NET_HPP__

#include <unistd.h>

#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


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


// Returns a Try of the hostname for the provided IP. If the hostname
// cannot be resolved, then a string version of the IP address is
// returned.
//
// TODO(benh): Merge with `net::hostname`.
inline Try<std::string> getHostname(const IP& ip)
{
  struct sockaddr_storage storage;
  memset(&storage, 0, sizeof(storage));

  switch (ip.family()) {
    case AF_INET: {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr = ip.in().get();
      addr.sin_port = 0;

      memcpy(&storage, &addr, sizeof(addr));
      break;
    }
    case AF_INET6: {
      struct sockaddr_in6 addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin6_family = AF_INET6;
      addr.sin6_addr = ip.in6().get();
      addr.sin6_port = 0;

      memcpy(&storage, &addr, sizeof(addr));
      break;
    }
    default: {
      ABORT("Unsupported family type: " + stringify(ip.family()));
    }
  }

  char hostname[MAXHOSTNAMELEN];
  socklen_t length;

  if (ip.family() == AF_INET) {
    length = sizeof(struct sockaddr_in);
  } else if (ip.family() == AF_INET6) {
    length = sizeof(struct sockaddr_in6);
  } else {
    return Error("Unknown address family: " + stringify(ip.family()));
  }

  int error = getnameinfo(
      (struct sockaddr*) &storage,
      length,
      hostname,
      MAXHOSTNAMELEN,
      nullptr,
      0,
      0);

  if (error != 0) {
    return Error(std::string(gai_strerror(error)));
  }

  return std::string(hostname);
}


// Returns a Try of the IP for the provided hostname or an error if no IP is
// obtained.
inline Try<IP> getIP(const std::string& hostname, int family = AF_UNSPEC)
{
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, family, 0);
  struct addrinfo* result = nullptr;

  int error = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  if (result->ai_addr == nullptr) {
    freeaddrinfo(result);
    return Error("No addresses found");
  }

  Try<IP> ip = IP::create(*result->ai_addr);

  if (ip.isError()) {
    freeaddrinfo(result);
    return Error("Unsupported family type");
  }

  freeaddrinfo(result);
  return ip.get();
}


// Returns the names of all the link devices in the system.
inline Try<std::set<std::string>> links()
{
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  std::set<std::string> names;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != nullptr) {
      names.insert(ifa->ifa_name);
    }
  }

  freeifaddrs(ifaddr);
  return names;
}


inline Try<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return ErrnoError();
  }

  struct addrinfo hints = createAddrInfo(SOCK_STREAM, AF_UNSPEC, AI_CANONNAME);
  struct addrinfo* result = nullptr;

  int error = getaddrinfo(host, nullptr, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  std::string hostname = result->ai_canonname;
  freeaddrinfo(result);

  return hostname;
}


// Returns a `Try` of the result of attempting to set the `hostname`.
inline Try<Nothing> setHostname(const std::string& hostname)
{
  if (sethostname(hostname.c_str(), hostname.size()) != 0) {
    return ErrnoError();
  }

  return Nothing();
}

} // namespace net {

#endif // __STOUT_POSIX_NET_HPP__
