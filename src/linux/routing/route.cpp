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

#include <stdint.h>

#include <netinet/in.h>

#include <netlink/addr.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/object.h>
#include <netlink/socket.h>

#include <netlink/route/route.h>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/stringify.hpp>

#include "linux/routing/internal.hpp"
#include "linux/routing/route.hpp"

#include "linux/routing/link/link.hpp"

using std::string;
using std::vector;

namespace routing {
namespace route {

Try<vector<Rule>> table()
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the routes (for IPv4) from kernel.
  struct nl_cache* c = nullptr;
  int error = rtnl_route_alloc_cache(socket->get(), AF_INET, 0, &c);
  if (error != 0) {
    return Error(nl_geterror(error));
  }

  Netlink<struct nl_cache> cache(c);

  vector<Rule> results;

  // Scan the routes and look for entries in the main routing table.
  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr; o = nl_cache_get_next(o)) {
    struct rtnl_route* route = (struct rtnl_route*) o;

    // TODO(jieyu): Currently, we assume each route in the routing
    // table has only one hop (which is true in most environments).
    if (rtnl_route_get_table(route) == RT_TABLE_MAIN &&
        rtnl_route_get_nnexthops(route) == 1) {
      CHECK_EQ(AF_INET, rtnl_route_get_family(route));

      // Get the destination IP network if exists.
      Option<net::IP::Network> destination;
      struct nl_addr* dst = rtnl_route_get_dst(route);
      if (dst != nullptr && nl_addr_get_len(dst) != 0) {
        struct in_addr* addr = (struct in_addr*) nl_addr_get_binary_addr(dst);
        Try<net::IP::Network> network = net::IP::Network::create(
            net::IP(*addr),
            nl_addr_get_prefixlen(dst));

        if (network.isError()) {
          return Error(
              "Invalid IP network format from the routing table: " +
              network.error());
        }

        destination = network.get();
      }

      // Get the default gateway if exists.
      Option<net::IP> gateway;
      struct rtnl_nexthop* hop = rtnl_route_nexthop_n(route, 0);
      struct nl_addr* gw = rtnl_route_nh_get_gateway(CHECK_NOTNULL(hop));
      if (gw != nullptr && nl_addr_get_len(gw) != 0) {
        struct in_addr* addr = (struct in_addr*) nl_addr_get_binary_addr(gw);
        gateway = net::IP(*addr);
      }

      // Get the destination link.
      int index = rtnl_route_nh_get_ifindex(hop);
      Result<string> link = link::name(index);
      if (link.isError()) {
        return Error("Failed to get the link name: " + link.error());
      } else if (link.isNone()) {
        return Error("Link of index " + stringify(index) + " is not found");
      }

      results.push_back(Rule(destination, gateway, link.get()));
    }
  }

  return results;
}


Result<net::IP> defaultGateway()
{
  Try<vector<Rule>> rules = table();
  if (rules.isError()) {
    return Error("Failed to get the routing table: " + rules.error());
  }

  foreach (const Rule& rule, rules.get()) {
    if (rule.destination.isNone() && rule.gateway.isSome()) {
      return rule.gateway.get();
    }
  }

  return None();
}

} // namespace route {
} // namespace routing {
