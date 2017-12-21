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

#ifndef __LOG_CATCHUP_HPP__
#define __LOG_CATCHUP_HPP__

#include <stdint.h>

#include <process/future.hpp>
#include <process/shared.hpp>

#include <stout/duration.hpp>
#include <stout/interval.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "log/network.hpp"
#include "log/replica.hpp"

namespace mesos {
namespace internal {
namespace log {

// Catches-up a set of log positions in the local replica. The user of
// this function can provide a hint on the proposal number that will
// be used for Paxos. This could potentially save us a few Paxos
// rounds. However, if the user has no idea what proposal number to
// use, they can just use none. We also allow the user to specify a
// timeout for the catch-up operation on each position and retry the
// operation if timeout happens. This can help us tolerate network
// blips.
extern process::Future<Nothing> catchup(
    size_t quorum,
    const process::Shared<Replica>& replica,
    const process::Shared<Network>& network,
    const Option<uint64_t>& proposal,
    const IntervalSet<uint64_t>& positions,
    const Duration& timeout = Seconds(10));


// Catches-up missing log positions in the local replica. Returns the
// highest position that was caught-up and now is safe to read. The
// user of this function can provide a hint on the proposal number
// that will be used for Paxos. We also allow the user to specify a
// timeout for the catch-up operation on each position and retry the
// operation if timeout happens.
process::Future<uint64_t> catchup(
    size_t quorum,
    const process::Shared<Replica>& replica,
    const process::Shared<Network>& network,
    const Option<uint64_t>& proposal = None(),
    const Duration& timeout = Seconds(10));

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_CATCHUP_HPP__
