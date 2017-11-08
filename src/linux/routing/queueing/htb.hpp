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

#ifndef __LINUX_ROUTING_QUEUEING_HTB_HPP__
#define __LINUX_ROUTING_QUEUEING_HTB_HPP__

#include <stdint.h>

#include <string>

#include <stout/bytes.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/try.hpp>

#include "linux/routing/handle.hpp"

namespace routing {
namespace queueing {
namespace htb {

constexpr char KIND[] = "htb";

struct DisciplineConfig
{
  DisciplineConfig(uint32_t _defcls = 0)
    : defcls(_defcls) {}

  // Default class.
  uint32_t defcls;
};

// Returns true if there exists an htb queueing discipline on the
// egress side of the link.
Try<bool> exists(
    const std::string& link,
    const Handle& parent);


// Creates a new htb queueing discipline on the egress side of
// the link. Returns false if a queueing discipline already exists which
// prevents the creation.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Option<Handle>& handle,
    const Option<DisciplineConfig>& config = None());


// Removes the htb queueing discipline from the link. Returns
// false if the htb queueing discipline is not found.
Try<bool> remove(
    const std::string& link,
    const Handle& parent);


// Returns the set of common Traffic Control statistics for the
// htb queueing discipline on the link, None() if the link or
// qdisc does not exist or an error if we cannot cannot determine the
// result.
Result<hashmap<std::string, uint64_t>> statistics(
    const std::string& link,
    const Handle& parent);


namespace cls {

struct Config
{
  Config(
      uint32_t _rate,
      Option<uint32_t> _ceil = None(),
      Option<uint32_t> _burst = None())
    : rate(_rate),
      ceil(_ceil),
      burst(_burst) {}

  Config() { rate = 0; }

  bool operator==(const Config& that) const
  {
    return rate == that.rate &&
           ceil == that.ceil &&
           burst == that.burst;
  }

  // Normal rate limit. The size of the normal rate bucket is not
  // exposed and will be computed by the kernel.
  uint32_t rate;
  // Burst limit.
  Option<uint32_t> ceil;
  // Size of the burst bucket.
  Option<uint32_t> burst;
};


void json(JSON::ObjectWriter* writer, const Config& config);


// Returns true if there exists an htb queueing class on the egress
// side of the link.
Try<bool> exists(
    const std::string& link,
    const Handle& classid);


// Creates a new htb queueing class on the link. Returns false if an
// htb queueing class already exists which prevents the creation.
Try<bool> create(
    const std::string& link,
    const Handle& parent,
    const Option<Handle> & handle,
    const Config& config);


// Update an existing htb queueing class on the link. Returns false if
// the htb queueing class is not found.
Try<bool> update(
    const std::string& link,
    const Handle& classid,
    const Config& config);


// Returns the configuration of the HTB class, None() if the link or
// class do not exist or an error if the result cannot be determined.
Result<Config> getConfig(
    const std::string& link,
    const Handle& classid);


// Removes the htb queueing class from the link. Returns false if the
// htb queueing class is not found.
Try<bool> remove(
    const std::string& link,
    const Handle& classid);

} // namespace cls {
} // namespace htb {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_HTB_HPP__
