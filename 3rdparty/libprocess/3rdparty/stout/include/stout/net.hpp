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

#ifndef __WINDOWS__
#include <netdb.h>
#endif // __WINDOWS__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif // __APPLE__

// Note: Header grouping and ordering is considered before
// inclusion/exclusion by platform.
#ifndef __WINDOWS__
#include <sys/param.h>

#include <curl/curl.h>
#endif // __WINDOWS__

#include <iostream>
#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/ip.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specifc system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/windows/net.hpp>
#else
#include <stout/posix/net.hpp>
#endif // __WINDOWS__


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


// TODO(evelinad): Move this to Address.
inline struct sockaddr_storage createSockaddrStorage(const IP& ip, int port)
{
  struct sockaddr_storage storage;
  memset(&storage, 0, sizeof(storage));

  switch (ip.family()) {
    case AF_INET: {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr = ip.in().get();
      addr.sin_port = htons(port);

      memcpy(&storage, &addr, sizeof(addr));
      break;
    }
    default: {
      ABORT("Unsupported family type: " + stringify(ip.family()));
    }
  }

  return storage;
}


inline Try<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return ErrnoError();
  }

  // TODO(evelinad): Add AF_UNSPEC when we will support IPv6.
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, AF_INET, AI_CANONNAME);
  struct addrinfo* result = NULL;

  int error = getaddrinfo(host, NULL, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  std::string hostname = result->ai_canonname;
  freeaddrinfo(result);

  return hostname;
}


// Returns a Try of the hostname for the provided IP. If the hostname
// cannot be resolved, then a string version of the IP address is
// returned.
inline Try<std::string> getHostname(const IP& ip)
{
  struct sockaddr_storage storage = createSockaddrStorage(ip, 0);
  char hostname[MAXHOSTNAMELEN];

  int error = getnameinfo(
      (struct sockaddr*) &storage,
      sizeof(storage),
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


// Returns the names of all the link devices in the system.
inline Try<std::set<std::string>> links()
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


// Returns a Try of the IP for the provided hostname or an error if no IP is
// obtained.
inline Try<IP> getIP(const std::string& hostname, int family)
{
  struct addrinfo hints = createAddrInfo(SOCK_STREAM, family, 0);
  struct addrinfo* result = NULL;

  int error = getaddrinfo(hostname.c_str(), NULL, &hints, &result);

  if (error != 0) {
    return Error(gai_strerror(error));
  }

  if (result->ai_addr == NULL) {
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

} // namespace net {

#endif // __STOUT_NET_HPP__
