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

#ifndef __LINUX_ROUTING_QUEUEING_INGRESS_HPP__
#define __LINUX_ROUTING_QUEUEING_INGRESS_HPP__

#include <stdint.h>

#include <string>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include "linux/routing/handle.hpp"

namespace routing {
namespace queueing {
namespace ingress {

constexpr char KIND[] = "ingress";


// The handle of the ingress queueing discipline is immutable.
constexpr Handle HANDLE = Handle(0xffff, 0);


// Returns true if there exists an ingress qdisc on the link.
Try<bool> exists(const std::string& link);


// Creates a new ingress qdisc on the link. Returns false if an
// ingress qdisc already exists on the link.
Try<bool> create(const std::string& link);


// Removes the ingress qdisc on the link. Return false if the ingress
// qdisc is not found.
Try<bool> remove(const std::string& link);

// Returns the set of common Traffic Control statistics for the
// ingress queueing discipline on the link, None() if the link or
// qdisc does not exist or an error if we cannot cannot determine the
// result.
Result<hashmap<std::string, uint64_t>> statistics(const std::string& link);

} // namespace ingress {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_INGRESS_HPP__
