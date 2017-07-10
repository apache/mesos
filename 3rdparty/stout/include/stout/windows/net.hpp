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

#ifndef __STOUT_WINDOWS_NET_HPP__
#define __STOUT_WINDOWS_NET_HPP__

#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")

#include <set>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>
#include <stout/windows/os.hpp>


namespace net {

inline struct addrinfoW createAddrInfo(int socktype, int family, int flags)
{
  struct addrinfoW addr;
  memset(&addr, 0, sizeof(addr));
  addr.ai_socktype = socktype;
  addr.ai_family = family;
  addr.ai_flags |= flags;

  return addr;
}


inline Error GaiError(int error)
{
  return Error(stringify(std::wstring(gai_strerrorW(error))));
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

  wchar_t hostname[MAXHOSTNAMELEN];
  socklen_t length;

  if (ip.family() == AF_INET) {
    length = sizeof(struct sockaddr_in);
  } else if (ip.family() == AF_INET6) {
    length = sizeof(struct sockaddr_in6);
  } else {
    return Error("Unknown address family: " + stringify(ip.family()));
  }

  int error = GetNameInfoW(
      (struct sockaddr*) &storage,
      length,
      hostname,
      MAXHOSTNAMELEN,
      nullptr,
      0,
      0);

  if (error != 0) {
    return GaiError(error);
  }

  return stringify(std::wstring(hostname));
}


// Returns a Try of the IP for the provided hostname or an error if no IP is
// obtained.
inline Try<IP> getIP(const std::string& hostname, int family = AF_UNSPEC)
{
  struct addrinfoW hints = createAddrInfo(SOCK_STREAM, family, 0);
  struct addrinfoW* result = nullptr;

  int error =
      GetAddrInfoW(wide_stringify(hostname).data(), nullptr, &hints, &result);

  if (error != 0) {
    return GaiError(error);
  }

  if (result->ai_addr == nullptr) {
    FreeAddrInfoW(result);
    return Error("No addresses found");
  }

  Try<IP> ip = IP::create(*result->ai_addr);

  if (ip.isError()) {
    FreeAddrInfoW(result);
    return Error("Unsupported family type");
  }

  FreeAddrInfoW(result);
  return ip.get();
}


// Returns the names of all the link devices in the system.
//
// NOTE: On Windows, the device names are GUID's which are not easily
// accessible via any command line tools.
//
// NOTE: This function only returns IPv4 info and does not return any
// info about the loopback interface.
inline Try<std::set<std::string>> links()
{
  DWORD result;
  ULONG size = 0;

  // Make an initial call to GetAdaptersInfo to get structure size.
  if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    return WindowsError("Calling GetAdaptersInfo returned unexpected result");
  }

  std::set<std::string> names;
  std::vector<IP_ADAPTER_INFO> adapter_info(size / sizeof(IP_ADAPTER_INFO));
  result = GetAdaptersInfo(
      static_cast<PIP_ADAPTER_INFO>(adapter_info.data()),
      &size);

  if (result != NO_ERROR) {
    return WindowsError(result, "GetAdaptersInfo failed");
  }

  foreach (const IP_ADAPTER_INFO& ip_adapter, adapter_info) {
    names.insert(ip_adapter.AdapterName);
  }

  return names;
}


inline Try<std::string> hostname()
{
  return os::internal::nodename();
}


// Returns a `Try` of the result of attempting to set the `hostname`.
inline Try<Nothing> setHostname(const std::string& hostname)
{
  if (::SetComputerNameW(wide_stringify(hostname).data()) == 0) {
    return WindowsError();
  }

  return Nothing();
}

} // namespace net {

#endif // __STOUT_WINDOWS_NET_HPP__
