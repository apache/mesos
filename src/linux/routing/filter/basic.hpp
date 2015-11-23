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

#ifndef __LINUX_ROUTING_FILTER_BASIC_HPP__
#define __LINUX_ROUTING_FILTER_BASIC_HPP__

#include <stdint.h>

#include <string>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include "linux/routing/handle.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

// This is a work around. Including <linux/if_ether.h> causes
// duplicated definitions on some platforms with old glibc.
#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif
#ifndef ETH_P_ARP
#define ETH_P_ARP 0x0806
#endif

namespace routing {
namespace filter {
namespace basic {

// The classifier for the basic filter only contains a protocol.
struct Classifier
{
  explicit Classifier(uint16_t _protocol)
    : protocol(_protocol) {}

  bool operator==(const Classifier& that) const
  {
    return protocol == that.protocol;
  }

  uint16_t protocol;
};


// Returns true if a basic packet filter with given protocol attached
// to the given parent exists on the link.
Try<bool> exists(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol);


// Creates a basic packet filter with given protocol attached to the
// given parent on the link which will set the classid for packets.
// Returns false if a basic packet filter with the given protocol
// attached to the given parent already exists on the link. The user
// can choose to specify an optional priority for the filter.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol,
    const Option<Priority>& priority,
    const Option<Handle>& classid);


// Creates a basic packet filter with given protocol attached to the
// given parent on the link which will redirect all matched packets to
// the target link. Returns false if a basic packet filter with the
// given protocol attached to the given parent already exists on the
// link. The user can choose to specify an optional priority for the
// filter.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol,
    const Option<Priority>& priority,
    const action::Redirect& redirect);


// Creates a basic packet filter with given protocol attached to the
// given parent on the link which will mirror all matched packets to a
// set of links (specified in the mirror action). Returns false if a
// basic packet filter with the give protocol attached to the given
// parent already exists on the link. The user can choose to specify
// an optional priority for the filter.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol,
    const Option<Priority>& priority,
    const action::Mirror& mirror);


// Removes the basic packet filter with given protocol attached to
// the parent from the link. Returns false if no basic packet filter
// with the given protocol attached to the given parent is found on
// the link.
Try<bool> remove(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol);


// Updates the action of a basic packet filter with give protocol
// attached to the given parent on the link. Returns false if no
// basic packet filter with the given protocol attached to the parent
// is found on the link.
Try<bool> update(
    const std::string& link,
    const Handle& parent,
    uint16_t protocol,
    const action::Mirror& mirror);

} // namespace basic {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_BASIC_HPP__
