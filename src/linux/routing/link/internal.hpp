// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __LINUX_ROUTING_LINK_INTERNAL_HPP__
#define __LINUX_ROUTING_LINK_INTERNAL_HPP__

#include <errno.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if.h> // Must be included after sys/socket.h.

#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/socket.h>

#include <netlink/route/link.h>

#include <string>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/internal.hpp"

namespace routing {

template <>
inline void cleanup(struct rtnl_link* link)
{
  rtnl_link_put(link);
}

namespace link {
namespace internal {

// Returns the netlink link object associated with a given link by its
// name. Returns None if the link is not found.
inline Result<Netlink<struct rtnl_link>> get(const std::string& link)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the netlink link objects from kernel. Note that the flag
  // AF_UNSPEC means all available families.
  struct nl_cache* c = nullptr;
  int error = rtnl_link_alloc_cache(socket->get(), AF_UNSPEC, &c);
  if (error != 0) {
    return Error(nl_geterror(error));
  }

  Netlink<struct nl_cache> cache(c);
  struct rtnl_link* l = rtnl_link_get_by_name(cache.get(), link.c_str());
  if (l == nullptr) {
    return None();
  }

  return Netlink<struct rtnl_link>(l);
}


// Returns the netlink link object associated with a given link by its
// interface index. Returns None if the link is not found.
inline Result<Netlink<struct rtnl_link>> get(int index)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the netlink link objects from kernel. Note that the flag
  // AF_UNSPEC means all available families.
  struct nl_cache* c = nullptr;
  int error = rtnl_link_alloc_cache(socket->get(), AF_UNSPEC, &c);
  if (error != 0) {
    return Error(nl_geterror(error));
  }

  Netlink<struct nl_cache> cache(c);
  struct rtnl_link* l = rtnl_link_get(cache.get(), index);
  if (l == nullptr) {
    return None();
  }

  return Netlink<struct rtnl_link>(l);
}


// Tests if the flags are set on the link. Returns None if the link is
// not found.
inline Result<bool> test(const std::string& _link, unsigned int flags)
{
  Result<Netlink<struct rtnl_link>> link = get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  return flags == (rtnl_link_get_flags(link->get()) & flags);
}


// Sets the flags on the link. Returns false if the link is not found.
inline Try<bool> set(const std::string& _link, unsigned int flags)
{
  Result<Netlink<struct rtnl_link>> link = get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  // TODO(jieyu): We use ioctl to set the flags because the interfaces
  // in libnl have some issues with virtual devices.
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));

  // Get the existing flags and take a bit-wise OR.
  ifr.ifr_flags = (rtnl_link_get_flags(link->get()) | flags);

  strncpy(ifr.ifr_name, _link.c_str(), IFNAMSIZ);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return ErrnoError();
  }

  if (ioctl(fd, SIOCSIFFLAGS, &ifr) == -1) {
    if (errno == ENODEV) {
      os::close(fd);
      return false;
    } else {
      // Save the error string as os::close may overwrite errno.
      const std::string message = os::strerror(errno);
      os::close(fd);
      return Error(message);
    }
  }

  os::close(fd);
  return true;
}

} // namespace internal {
} // namespace link {
} // namespace routing {

#endif // __LINUX_ROUTING_LINK_INTERNAL_HPP__
