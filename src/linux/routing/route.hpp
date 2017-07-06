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

#ifndef __LINUX_ROUTING_ROUTE_HPP__
#define __LINUX_ROUTING_ROUTE_HPP__

#include <string>
#include <vector>

#include <stout/ip.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

namespace routing {
namespace route {

// Represents a rule in the routing table (for IPv4).
struct Rule
{
  Rule(const Option<net::IP::Network>& _destination,
       const Option<net::IP>& _gateway,
       const std::string& _link)
    : destination(_destination),
      gateway(_gateway),
      link(_link) {}

  Option<net::IP::Network> destination;
  Option<net::IP> gateway;
  std::string link;
};


// Returns the main routing table of this host.
Try<std::vector<Rule>> table();


// Returns the default gateway of this host.
Result<net::IP> defaultGateway();

} // namespace route {
} // namespace routing {

#endif // __LINUX_ROUTING_ROUTE_HPP__
