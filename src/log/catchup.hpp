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

#ifndef __LOG_CATCHUP_HPP__
#define __LOG_CATCHUP_HPP__

#include <stdint.h>

#include <set>

#include <process/future.hpp>
#include <process/shared.hpp>

#include <stout/nothing.hpp>

#include "log/network.hpp"
#include "log/replica.hpp"

namespace mesos {
namespace internal {
namespace log {

// Catches-up a set of log positions in the local replica. The user of
// this function can provide a hint on the proposal number that will
// be used for Paxos. This could potentially save us a few Paxos
// rounds. However, if the user has no idea what proposal number to
// use, he can just use an arbitrary proposal number (e.g., 0).
extern process::Future<Nothing> catchup(
    size_t quorum,
    const process::Shared<Replica>& replica,
    const process::Shared<Network>& network,
    uint64_t proposal,
    const std::set<uint64_t>& positions);

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_CATCHUP_HPP__
