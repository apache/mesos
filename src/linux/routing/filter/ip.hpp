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

#ifndef __LINUX_ROUTING_FILTER_IP_HPP__
#define __LINUX_ROUTING_FILTER_IP_HPP__

#include <stdint.h>

#include <iosfwd>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>

#include <stout/ip.hpp>
#include <stout/net.hpp>
#include <stout/mac.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/handle.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

namespace routing {
namespace filter {
namespace ip {

// Represents a port range that can be used by a single u32 matcher.
// The port range [begin, end] (both begin and end are inclusive)
// should have size = 2^n (n=0,1,2,...) and its begin is size aligned
// (i.e., begin % size == 0).
class PortRange
{
public:
  // Creates a port range from the specified begin and end. Returns
  // error if it does not meet the above requirements. All values are
  // in host order.
  static Try<PortRange> fromBeginEnd(uint16_t begin, uint16_t end);

  // Creates a port range from the specified begin and mask. Returns
  // error if it does not meet the above requirements. All values are
  // in host order.
  static Try<PortRange> fromBeginMask(uint16_t begin, uint16_t mask);

  // Returns the begin (in host order) of this port range.
  uint16_t begin() const { return begin_; }

  // Returns the end (in host order) of this port range.
  uint16_t end() const { return end_; }

  // Returns the mask (in host order) of this port range.
  uint16_t mask() const { return ~(end_ - begin_); }

  bool operator==(const PortRange& that) const
  {
    return begin_ == that.begin_ && end_ == that.end_;
  }

private:
  PortRange(uint16_t _begin, uint16_t _end)
    : begin_(_begin), end_(_end) {}

  uint16_t begin_; // In host order.
  uint16_t end_;   // In host order.
};


std::ostream& operator<<(std::ostream& stream, const PortRange& range);


struct Classifier
{
  Classifier(
    const Option<net::MAC>& _destinationMAC,
    const Option<net::IP>& _destinationIP,
    const Option<PortRange>& _sourcePorts,
    const Option<PortRange>& _destinationPorts)
    : destinationMAC(_destinationMAC),
      destinationIP(_destinationIP),
      sourcePorts(_sourcePorts),
      destinationPorts(_destinationPorts) {}

  bool operator==(const Classifier& that) const
  {
    return (destinationMAC == that.destinationMAC &&
        destinationIP == that.destinationIP &&
        destinationPorts == that.destinationPorts &&
        sourcePorts == that.sourcePorts);
  }

  Option<net::MAC> destinationMAC;

  // TODO(evelinad): Replace net::IP with net::IP::Network when we will
  // support classifiers for the entire subnet.
  Option<net::IP> destinationIP;

  Option<PortRange> sourcePorts;
  Option<PortRange> destinationPorts;
};


// Returns true if an IP packet filter attached to the given parent
// that matches the specified classifier exists on the link.
Try<bool> exists(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier);


// Creates an IP packet filter attached to the given parent on the
// link which will redirect all the IP packets that satisfy the
// conditions specified by the classifier to the target link. Returns
// false if an IP packet filter attached to the given parent with the
// same classifier already exists.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Redirect& redirect);


// Same as above, but allow the user to specify the handle. This
// interface is exposed only for testing MESOS-1617.
// TODO(jieyu): Revisit this once the kernel bug is fixed.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const Option<Handle>& handle,
    const action::Redirect& redirect);


// Creates an IP packet filter attached to the given parent on the
// link which will stop the IP packets from being sent to the next
// filter. Returns false if an IP packet filter attached to the given
// parent with the same classifier already exists.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Terminal& terminal);


// Creates an IP packet filter attached to the given parent on the
// link which will set the classid for packets and stop the IP packets
// from being sent to the next filter. Returns false if an IP packet
// filter attached to the given parent with the same classifier
// already exists.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const Option<Handle>& classid);


// Removes the IP packet filter attached to the given parent that
// matches the specified classifier from the link. Returns false if
// such a filter is not found.
Try<bool> remove(
    const std::string& link,
    const Handle& parent,
    const Classifier& classifier);


// Returns all the IP packet filters attached to the given parent on
// the link. Returns none if the link or the parent is not found.
Result<std::vector<Filter<Classifier>>> filters(
    const std::string& link,
    const Handle& parent);


// Returns the classifiers of all the IP packet filters attached to
// the given parent on the link. Returns none if the link or the
// parent is not found.
Result<std::vector<Classifier>> classifiers(
    const std::string& link,
    const Handle& parent);

} // namespace ip {
} // namespace filter {
} // namespace routing {

namespace std {

template <>
struct hash<routing::filter::ip::PortRange>
{
  typedef size_t result_type;

  typedef routing::filter::ip::PortRange argument_type;

  result_type operator()(const argument_type& range) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, range.begin());
    boost::hash_combine(seed, range.end());
    return seed;
  }
};

} // namespace std {

#endif // __LINUX_ROUTING_FILTER_IP_HPP__
