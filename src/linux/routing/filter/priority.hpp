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

#ifndef __LINUX_ROUTING_FILTER_PRIORITY_HPP__
#define __LINUX_ROUTING_FILTER_PRIORITY_HPP__

#include <stdint.h>

namespace routing {
namespace filter {

// Each filter on a link has a priority (a 16-bit integer). The
// priority defines the order in which filters will be applied to each
// packet. The lower the number, the higher the priority. In order to
// deal with filters of different types, we split the priority into
// two 8-bit parts: a primary part and a secondary part. When
// comparing two priorities, their primary parts will be compared
// first. If they have the same primary part, their secondary parts
// will then be compared. Typically, the primary part is used to
// define the preference order between different types of filters. For
// example, a user may want ICMP packet filters to be always applied
// first than IP packet filters. In that case, the user can assign
// ICMP packet filters a higher priority in the primary part. If the
// user wants to further define orders between filters of the same
// type, they can use the secondary part.
class Priority
{
public:
  explicit Priority(uint16_t priority)
  {
    primary = (uint8_t) (priority >> 8);
    secondary = (uint8_t) priority;
  }

  Priority(uint8_t _primary, uint8_t _secondary)
    : primary(_primary), secondary(_secondary) {}

  uint16_t get() const
  {
    return (((uint16_t) primary) << 8) + secondary;
  }

private:
  uint8_t primary;
  uint8_t secondary;
};

} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_PRIORITY_HPP__
