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

#ifndef __STOUT_WINDOWS_MAC_HPP__
#define __STOUT_WINDOWS_MAC_HPP__

#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")

#include <algorithm>
#include <vector>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>


// Network utilities.
namespace net {

// Returns the MAC address of a given link device. The link device is
// specified using its name. Returns an error if the link device is not found.
inline Result<MAC> mac(const std::string& name)
{
  DWORD result;
  ULONG size = 0;

  // Indicates whether the link device is found or not.
  bool found = false;

  // Make an initial call to GetAdaptersInfo to get structure size.
  if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    return WindowsError("Calling GetAdaptersInfo returned unexpected result");
  }

  std::vector<IP_ADAPTER_INFO> adapterInfo(size / sizeof(IP_ADAPTER_INFO));
  result = GetAdaptersInfo(
      static_cast<PIP_ADAPTER_INFO>(adapterInfo.data()),
      &size);

  if (result != NO_ERROR) {
    return WindowsError(result, "GetAdaptersInfo failed");
  }

  foreach (const IP_ADAPTER_INFO& ip_adapter, adapterInfo) {
    if (!strcmp(ip_adapter.AdapterName, name.c_str())) {
      found = true;

      if (ip_adapter.AddressLength == 6) {
        // NOTE: Although the `AddressLength` is 6, the size of the
        // `Address` byte buffer is usually larger than required, which
        // means we need to copy the bytes before the `MAC` constructor
        // will accept them.
        uint8_t mac_buffer[6];
        std::copy(ip_adapter.Address, ip_adapter.Address + 6, mac_buffer);
        MAC mac(mac_buffer);

        // Ignore if the address is 0 so that the results are
        // consistent across all platforms.
        if (stringify(mac) == "00:00:00:00:00:00") {
          continue;
        }

        return mac;
      }
    }
  }

  if (!found) {
    return Error("Cannot find the link device");
  }

  return None();
}

} // namespace net {

#endif // __STOUT_WINDOWS_MAC_HPP__
