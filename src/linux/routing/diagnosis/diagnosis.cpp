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

#include <netlink/cache.h>
#include <netlink/errno.h>

#include <netlink/idiag/msg.h>

#include <stout/error.hpp>
#include <stout/try.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/diagnosis/diagnosis.hpp"

using namespace std;

namespace routing {

template <>
inline void cleanup(struct idiagnl_msg* msg)
{
  idiagnl_msg_put(msg);
}

namespace diagnosis {
namespace socket {

static Option<net::IP> IP(nl_addr* _ip)
{
  if (_ip != nullptr && nl_addr_get_len(_ip) != 0) {
    if (nl_addr_get_family(_ip) == AF_INET) {
      struct in_addr* addr = (struct in_addr*)nl_addr_get_binary_addr(_ip);
      return net::IP(*addr);
    }

    if (nl_addr_get_family(_ip) == AF_INET6) {
      struct in6_addr* addr = (struct in6_addr*)nl_addr_get_binary_addr(_ip);
      return net::IP(*addr);
    }
  }

  return None();
}


Try<vector<Info>> infos(int family, int states)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket(NETLINK_INET_DIAG);
  if (socket.isError()) {
    return Error(socket.error());
  }

  struct nl_cache* c = nullptr;
  int error = idiagnl_msg_alloc_cache(socket->get(), family, states, &c);
  if (error != 0) {
    return Error(nl_geterror(error));
  }

  Netlink<struct nl_cache> cache(c);

  vector<Info> results;
  results.reserve(nl_cache_nitems(cache.get()));
  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr; o = nl_cache_get_next(o)) {
    struct idiagnl_msg* msg = (struct idiagnl_msg*)o;

    // For 'state', libnl-idiag only returns the number of left
    // shifts. Convert it back to power-of-2 number.

    results.emplace_back(
        idiagnl_msg_get_family(msg),
        1 << idiagnl_msg_get_state(msg),
        idiagnl_msg_get_inode(msg),
        idiagnl_msg_get_sport(msg),
        idiagnl_msg_get_dport(msg),
        IP(idiagnl_msg_get_src(msg)),
        IP(idiagnl_msg_get_dst(msg)),
        idiagnl_msg_get_tcpinfo(msg));
  }

  return results;
}

} // namespace socket {
} // namespace diagnosis {
} // namespace routing {
