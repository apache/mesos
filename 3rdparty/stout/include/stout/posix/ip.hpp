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

#ifndef __STOUT_POSIX_IP_HPP__
#define __STOUT_POSIX_IP_HPP__

#include <ifaddrs.h>

#include <sys/types.h>

#include <string>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>


namespace net {

inline Result<IP::Network> IP::Network::fromLinkDevice(
    const std::string& name,
    int family)
{
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

          Try<IP::Network> network = IP::Network::create(address, netmask);
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

        Try<IP::Network> network = IP::Network::create(address, prefix);
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
}

} // namespace net {

#endif // __STOUT_POSIX_IP_HPP__
