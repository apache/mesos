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

#ifndef __LINUX_ROUTING_FILTER_IP_HPP__
#define __LINUX_ROUTING_FILTER_IP_HPP__

#include <stdint.h>

#include <iostream>
#include <string>
#include <vector>

#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/queueing/handle.hpp"

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

  bool operator == (const PortRange& that) const
  {
    return begin_ == that.begin_ && end_ == that.end_;
  }

private:
  PortRange(uint16_t _begin, uint16_t _end)
    : begin_(_begin), end_(_end) {}

  uint16_t begin_; // In host order.
  uint16_t end_;   // In host order.
};


inline std::ostream& operator << (
    std::ostream& stream,
    const PortRange& range)
{
  return stream << "[" << range.begin() << "," << range.end() << "]";
}


class Classifier
{
public:
  Classifier(
    const Option<net::MAC>& _destinationMAC,
    const Option<net::IP>& _destinationIP,
    const Option<PortRange>& _sourcePorts,
    const Option<PortRange>& _destinationPorts)
    : destinationMAC_(_destinationMAC),
      destinationIP_(_destinationIP),
      sourcePorts_(_sourcePorts),
      destinationPorts_(_destinationPorts) {}

  bool operator == (const Classifier& that) const
  {
    return (destinationMAC_ == that.destinationMAC_ &&
        destinationIP_ == that.destinationIP_ &&
        destinationPorts_ == that.destinationPorts_ &&
        sourcePorts_ == that.sourcePorts_);
  }

  const Option<net::MAC>& destinationMAC() const { return destinationMAC_; }
  const Option<net::IP>& destinationIP() const { return destinationIP_; }
  const Option<PortRange>& sourcePorts() const { return sourcePorts_; }

  const Option<PortRange>& destinationPorts() const
  {
    return destinationPorts_;
  }

private:
  Option<net::MAC> destinationMAC_;
  Option<net::IP> destinationIP_;
  Option<PortRange> sourcePorts_;
  Option<PortRange> destinationPorts_;
};


// Returns true if an IP packet filter attached to the given parent
// that matches the specified classifier exists on the link.
Try<bool> exists(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier);


// Creates an IP packet filter attached to the given parent on the
// link which will redirect all the IP packets that satisfy the
// conditions specified by the classifier to the target link. Returns
// false if an IP packet filter attached to the given parent with the
// same classifier already exists.
Try<bool> create(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Redirect& redirect);


// Removes the IP packet filter attached to the given parent that
// matches the specified classifier from the link. Returns false if
// such a filter is not found.
Try<bool> remove(
    const std::string& link,
    const queueing::Handle& parent,
    const Classifier& classifier);


// Returns the classifiers of all the IP packet filters attached to
// the given parent on the link. Returns none if the link or the
// parent is not found.
Result<std::vector<Classifier> > classifiers(
    const std::string& link,
    const queueing::Handle& parent);

} // namespace ip {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_IP_HPP__
