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

#ifndef __LINUX_ROUTING_LINK_LINK_HPP__
#define __LINUX_ROUTING_LINK_LINK_HPP__

#include <stdint.h>

#include <string>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/mac.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

namespace routing {
namespace link {

// Returns the name of the public facing interface of the host (
// 'eth0' on most machines). The interface returned is the first
// interface in the routing table that has an empty 'destination'.
// Returns None if such an interface cannot be found.
Result<std::string> eth0();


// Returns the name of the loopback interface of the host (the 'lo'
// device on most machines). The interface returned has flag
// IFF_LOOPBACK set on it. Returns None if no loopback interface can
// be found on the host.
Result<std::string> lo();


// Returns true if the link exists.
Try<bool> exists(const std::string& link);


// Removes a link. Returns false if the link is not found.
Try<bool> remove(const std::string& link);


// Waits for the link to be removed. The returned future will be set
// once the link has been removed. The user can discard the returned
// future to cancel the operation.
process::Future<Nothing> removed(const std::string& link);


// Returns the interface index of the link. Returns None if the link
// is not found.
Result<int> index(const std::string& link);


// Returns the name of the link from its interface index. Returns None
// if the link with the given interface index is not found.
Result<std::string> name(int index);


// Returns true if the link is up. Returns None if the link is not
// found.
Result<bool> isUp(const std::string& link);


// Sets the link up (IFF_UP). Returns false if the link is not found.
Try<bool> setUp(const std::string& link);


// Sets the MAC address of the link. Returns false if the link is not
// found.
Try<bool> setMAC(const std::string& link, const net::MAC& mac);


// Returns the Maximum Transmission Unit (MTU) of the link. Returns
// None if the link is not found.
Result<unsigned int> mtu(const std::string& link);


// Sets the Maximum Transmission Unit (MTU) of the link. Returns false
// if the link is not found.
Try<bool> setMTU(const std::string& link, unsigned int mtu);


// Returns the statistics of the link.
Result<hashmap<std::string, uint64_t>> statistics(const std::string& link);

} // namespace link {
} // namespace routing {

#endif // __LINUX_ROUTING_LINK_LINK_HPP__
