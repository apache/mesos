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

#ifndef __LOG_RECOVER_HPP__
#define __LOG_RECOVER_HPP__

#include <stdint.h>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/nothing.hpp>

#include "log/network.hpp"
#include "log/replica.hpp"

namespace mesos {
namespace internal {
namespace log {

// Runs the recover protocol. We will re-run the recover protocol if
// it cannot be finished within 'timeout'.
process::Future<Option<RecoverResponse>> runRecoverProtocol(
    size_t quorum,
    const process::Shared<Network>& network,
    const Metadata::Status& status,
    bool autoInitialize,
    const Duration& timeout = Seconds(10));


// Recovers a replica by catching up enough missing positions. A
// replica starts with an empty log (e.g., in the case of a disk
// failure) should not be allowed to vote. Otherwise, the new votes it
// makes may contradict its lost votes, leading to potential
// inconsistency in the log. Instead, the replica should be put in
// non-voting status and catch up missing positions (and associated
// Paxos states). The replica can be re-allowed to vote if the
// following two conditions are met: 1) a sufficient amount of missing
// positions are recovered such that if other replicas fail, the
// remaining replicas can restore all the successfully written log
// entries; 2) its future votes cannot not contradict its lost votes.
//
// This function returns an owned pointer to the recovered replica if
// the recovery is successful. If the auto-initialization flag is set,
// an empty replica will be allowed to vote if ALL replicas (i.e.,
// quorum * 2 - 1) are empty. This allows us to bootstrap the
// replicated log without explicitly using an initialization tool.
extern process::Future<process::Owned<Replica>> recover(
    size_t quorum,
    const process::Owned<Replica>& replica,
    const process::Shared<Network>& network,
    bool autoInitialize = false);

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_RECOVER_HPP__
