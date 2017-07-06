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

#ifndef __STOUT_WINDOWS_IP_HPP__
#define __STOUT_WINDOWS_IP_HPP__

#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")

#include <string>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>


namespace net {

inline Result<IP::Network> IP::Network::fromLinkDevice(
    const std::string& name,
    int family)
{
  DWORD result;
  ULONG size = 0;

  // TODO(josephw): `GetAdaptersInfo` will only return IPv4 results.
  // To get IPv6 results, we should look into `GetAdaptersAddresses`.
  if (family != AF_INET) {
    return Error("Unsupported family type: " + stringify(family));
  }

  // Indicates whether the link device is found or not.
  bool found = false;

  // Make an initial call to GetAdaptersInfo to get structure size.
  if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    return WindowsError("Calling GetAdaptersInfo returned unexpected result");
  }

  std::vector<IP_ADAPTER_INFO> adapter_info(size / sizeof(IP_ADAPTER_INFO));
  result = GetAdaptersInfo(
      static_cast<PIP_ADAPTER_INFO>(adapter_info.data()),
      &size);

  if (result != NO_ERROR) {
    return WindowsError(result, "GetAdaptersInfo failed");
  }

  foreach(const IP_ADAPTER_INFO& ip_adapter, adapter_info) {
    if (!strcmp(ip_adapter.AdapterName, name.c_str())) {
      found = true;

      IP address = IP::parse(ip_adapter.IpAddressList.IpAddress.String).get();

      if (ip_adapter.IpAddressList.IpMask.String[0] != '\0') {
        IP netmask = IP::parse(ip_adapter.IpAddressList.IpMask.String).get();

        Try<IP::Network> network = IP::Network::create(address, netmask);
        if (network.isError()) {
           return Error(network.error());
        }

        return network.get();
      }

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

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
}

} // namespace net {

#endif // __STOUT_WINDOWS_IP_HPP__
