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

#ifndef __LINUX_ROUTING_QUEUEING_HANDLE_HPP__
#define __LINUX_ROUTING_QUEUEING_HANDLE_HPP__

#include <stdint.h>

namespace routing {
namespace queueing {

// Represents a handle for a queueing object (either a queueing
// discipline or a queueing class). It can be specified by combining a
// primary number and a secondary number (modeled after traffic
// control object handle used in kernel).
class Handle
{
public:
  explicit Handle(uint32_t _handle) : handle(_handle) {}

  Handle(uint16_t primary, uint16_t secondary)
  {
    handle = (((uint32_t) primary) << 16) + secondary;
  }

  // NOTE: This is used to construct a classid. The higher 16 bits of
  // the given 'parent' will be the primary and the lower 16 bits is
  // specified by the given 'id'.
  Handle(Handle parent, uint16_t id)
  {
    handle = (((uint32_t) parent.primary()) << 16) + id;
  }

  uint16_t primary() const { return handle >> 16; }
  uint16_t secondary() const { return handle & 0x0000ffff; }
  uint32_t get() const { return handle; }

private:
  uint32_t handle;
};


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


// The parent of the root ingress queueing discipline.
extern const Handle INGRESS_ROOT;


// The parent of the root egress queueing discipline.
extern const Handle EGRESS_ROOT;

} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_HANDLE_HPP__
