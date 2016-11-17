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

#ifndef __LINUX_ROUTING_HANDLE_HPP__
#define __LINUX_ROUTING_HANDLE_HPP__

#include <stdint.h>

#include <iosfwd>

#include <netlink/route/tc.h>

#include <stout/try.hpp>

namespace routing {

// The Linux kernel Traffic Control (TC) uses handles to uniqely
// identify the queueing disciplines (qdiscs), classes and filters
// attached to a network interface. The most common type of handle is
// identified by primary and secondary device numbers (sometimes
// called major and minor numbers) and written as primary:secondary.
// Handles provide the mechanism by which TC classes, qdiscs and
// filters can be connected together to create complex network traffic
// policing policies.
class Handle
{
public:
  explicit constexpr Handle(uint32_t _handle) : handle(_handle) {}

  constexpr Handle(uint16_t primary, uint16_t secondary)
    : handle((((uint32_t) primary) << 16) + secondary) {}

  // NOTE: This is used to construct a classid. The higher 16 bits of
  // the given 'parent' will be the primary and the lower 16 bits is
  // specified by the given 'id'.
  constexpr Handle(const Handle& parent, uint16_t id)
    : handle((((uint32_t) parent.primary()) << 16) + id) {}

  constexpr bool operator==(const Handle& that) const
  {
    return handle == that.handle;
  }

  constexpr bool operator!=(const Handle& that) const
  {
    return handle != that.handle;
  }

  static Try<Handle> parse(const std::string& str);

  constexpr uint16_t primary() const { return handle >> 16; }
  constexpr uint16_t secondary() const { return handle & 0x0000ffff; }
  constexpr uint32_t get() const { return handle; }

protected:
  uint32_t handle;
};


std::ostream& operator<<(std::ostream& stream, const Handle& handle);


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
// The root handles for both ingress and egress are immutable.
constexpr Handle EGRESS_ROOT = Handle(TC_H_ROOT);
constexpr Handle INGRESS_ROOT = Handle(TC_H_INGRESS);

} // namespace routing {

#endif // __LINUX_ROUTING_HANDLE_HPP__
