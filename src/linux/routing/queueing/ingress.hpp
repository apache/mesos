/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LINUX_ROUTING_QUEUEING_INGRESS_HPP__
#define __LINUX_ROUTING_QUEUEING_INGRESS_HPP__

#include <string>

#include <stout/try.hpp>

#include "linux/routing/handle.hpp"

namespace routing {
namespace queueing {
namespace ingress {

// Packets flowing from the device driver to the network stack are
// called ingress traffic, and packets flowing from the network stack
// to the device driver are called egress traffic (shown below).
//
//        +---------+
//        | Network |
//        |  Stack  |
//        |---------|
//        |  eth0   |
//        +---------+
//           ^   |
//   Ingress |   | Egress
//           |   |
//    -------+   +------>
//
// For the ingress traffic, there are two immutable handles defined
// for the interface which specify the root handle under which a
// queueing discipline can be created, and the handle of any created
// ingress filter.
extern const Handle ROOT;
extern const Handle HANDLE;


// Returns true if there exists an ingress qdisc on the link.
Try<bool> exists(const std::string& link);


// Creates a new ingress qdisc on the link. Returns false if an
// ingress qdisc already exists on the link.
Try<bool> create(const std::string& link);


// Removes the ingress qdisc on the link. Return false if the ingress
// qdisc is not found.
Try<bool> remove(const std::string& link);

} // namespace ingress {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_INGRESS_HPP__
