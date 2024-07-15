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

#include <unistd.h>

#include <netlink/errno.h>

#include <netlink/route/link/veth.h>
#include <netlink/route/link.h>
#include <netlink/netlink.h>

#include <stout/error.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/link/veth.hpp"

using std::string;

namespace routing {
namespace link {
namespace veth {

Try<bool> create(
    const string& veth,
    const string& peer,
    const Option<pid_t>& pid,
    const Option<net::MAC>& veth_mac,
    const Option<net::MAC>& peer_mac)
{
  auto set_addr_from_mac = [](struct rtnl_link* link, const net::MAC& mac)
  {
    struct nl_addr* addr;
    unsigned char mac_addr_uchar[6];
    for (int i = 0; i < 6; i++) {
      mac_addr_uchar[i] = (unsigned char) mac[i];
    }
    addr = nl_addr_build(AF_LLC, mac_addr_uchar, 6);
    rtnl_link_set_addr(link, addr);
    return addr;
  };

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  struct nl_sock *sock = socket->get();
  struct rtnl_link *veth_link, *peer_link;
  int error = -NLE_NOMEM;

  if (!(veth_link = rtnl_link_veth_alloc())) {
    return Error(nl_geterror(error));
  }
  peer_link = rtnl_link_veth_get_peer(veth_link);

  Option<struct nl_addr*> veth_addr, peer_addr;
  if (veth_mac.isSome()) {
    veth_addr = set_addr_from_mac(veth_link, *veth_mac);
  }
  if (peer_mac.isSome()) {
    peer_addr = set_addr_from_mac(peer_link, *peer_mac);
  }

  rtnl_link_set_name(veth_link, veth.c_str());
  rtnl_link_set_name(peer_link, peer.c_str());

  rtnl_link_set_ns_pid(peer_link, pid.getOrElse(getpid()));
  error = rtnl_link_add(sock, veth_link, NLM_F_CREATE | NLM_F_EXCL);

  rtnl_link_put(veth_link);
  if (veth_addr.isSome()) {
    nl_addr_put(*veth_addr);
  }
  if (peer_addr.isSome()) {
    nl_addr_put(*peer_addr);
  }

  if (error != 0) {
    if (error == -NLE_EXIST) {
      return false;
    }
    return Error(nl_geterror(error));
  }

  return true;
}

} // namespace veth {
} // namespace link {
} // namespace routing {
