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

#ifndef __LINUX_ROUTING_FILTER_ARP_HPP__
#define __LINUX_ROUTING_FILTER_ARP_HPP__

#include <string>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/queueing/handle.hpp"

namespace routing {
namespace filter {
namespace arp {

// Returns true if an ARP packet filter attached to the given parent
// exists on the link.
Try<bool> exists(const std::string& link, const queueing::Handle& parent);


// Creates an ARP packet filter attached to the given parent on the
// link which will redirect all ARP packets to the target link.
// Returns false if an ARP packet filter attached to the given parent
// already exists on the link. The user can choose to specify an
// optional priority for the filter.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Option<Priority>& priority,
    const action::Redirect& redirect);


// Creates an ARP packet filter attached to the given parent on the
// link which will mirror all ARP packets to a set of links (specified
// in the mirror action). Returns false if an ARP packet filter
// attached to the given parent already exists on the link. The user
// can choose to specify an optional priority for the filter.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Option<Priority>& priority,
    const action::Mirror& mirror);


// Creates an ARP packet filter attached to the given parent on the
// link which will set the classid for packets. Returns false if an
// ARP packet filter attached to the given parent already exists on
// the link. The user can choose to specify an optional priority for
// the filter.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Option<Priority>& priority,
    const Option<queueing::Handle>& classid);


// Removes the ARP packet filter attached to the parent from the link.
// Returns false if no ARP packet filter attached to the given parent
// is found on the link.
Try<bool> remove(const std::string& link, const queueing::Handle& parent);


// Updates the action of the APR packet filter attached to the given
// parent on the link. Returns false if no ARP packet filter attached
// to the parent is found on the link.
Try<bool> update(
    const std::string& link,
    const queueing::Handle& parent,
    const action::Mirror& mirror);

} // namespace arp {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_ARP_HPP__
