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

#ifndef __LINUX_ROUTING_FILTER_HANDLE_HPP__
#define __LINUX_ROUTING_FILTER_HANDLE_HPP__

#include <stdint.h>

#include "linux/routing/handle.hpp"

namespace routing {
namespace filter {

// When the number of Linux kernel Traffic Control (TC) objects
// attached to an interface is high, the kernel can spend a
// significant amount of time looking up TC filters (an operation that
// must be performed for every packet on the interface). To speed up
// these lookups, an alternative interpretation of the handle bits,
// the U32Handle, can be used breaking down the identifier into hash
// table id (htid), bucket (hash) and filter item (node) within the
// bucket. Careful selection of the handles by the administrator
// allows for the construction of hash tables that can significantly
// reduce lookup times.
// See http://ace-host.stuart.id.au/russell/files/tc/doc/cls_u32.txt
class U32Handle : public Handle
{
public:
  explicit U32Handle(uint32_t _handle) : Handle(_handle) {}

  // The format of a u32 filter handle.
  // +------------+--------+------------+
  // |   htid     |  hash  |    node    |
  // +------------+--------+------------+
  //     12 bits    8 bits     12 bits
  // NOLINT(readability/ending_punctuation)
  U32Handle(uint32_t htid, uint32_t hash, uint32_t node)
    : Handle(((htid & 0xfff) << 20) + ((hash & 0xff) << 12) + (node & 0xfff)) {}

  virtual ~U32Handle() {}

  uint32_t htid() const { return handle >> 20; }
  uint32_t hash() const { return (handle & 0x000ff000) >> 12; }
  uint32_t node() const { return handle & 0x00000fff; }
};

} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_HANDLE_HPP__
