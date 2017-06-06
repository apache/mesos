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

#ifndef __STOUT_POSIX_MAC_HPP__
#define __STOUT_POSIX_MAC_HPP__

#include <ifaddrs.h>

#include <sys/types.h>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>


// Network utilities.
namespace net {

// Returns the MAC address of a given link device. The link device is
// specified using its name (e.g., eth0). Returns error if the link
// device is not found. Returns none if the link device is found, but
// does not have a MAC address (e.g., loopback).
inline Result<MAC> mac(const std::string& name)
{
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return ErrnoError();
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != nullptr && !strcmp(ifa->ifa_name, name.c_str())) {
      found = true;

# if defined(__linux__)
      if (ifa->ifa_addr != nullptr && ifa->ifa_addr->sa_family == AF_PACKET) {
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
      if (ifa->ifa_addr != nullptr && ifa->ifa_addr->sa_family == AF_LINK) {
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
}

} // namespace net {

#endif // __STOUT_POSIX_MAC_HPP__
