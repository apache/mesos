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

#ifndef __LOG_CONSENSUS_HPP__
#define __LOG_CONSENSUS_HPP__

#include <stdint.h>

#include <process/future.hpp>
#include <process/shared.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "log/network.hpp"

#include "messages/log.hpp"

// We use Paxos consensus protocol to agree on the value of each entry
// in the replicated log. In our system, each replica is both an
// acceptor and a learner. There are several types of proposers in the
// system. Coordinator is one type of proposers we use to append new
// log entries. The 'log::fill' function below creates an internal
// proposer each time it is called. These internal proposers are used
// to agree on previously written entries in the log.

namespace mesos {
namespace internal {
namespace log {

// Runs the promise phase (a.k.a., the prepare phase) in Paxos. This
// phase has two purposes. First, the proposer asks promises from a
// quorum of replicas not to accept writes from proposers with lower
// proposal numbers. Second, the proposer looks for potential
// previously agreed values. Only these values can be written in the
// next phase. This restriction is used by Paxos to make sure that if
// a value has been agreed on for a log position, subsequent writes to
// this log position will always have the same value. We can run the
// promise phase either for a specified log position ("explicit"
// promise), or for all positions that have not yet been promised to
// any proposer ("implicit" promise). The latter is a well known
// optimization called Multi-Paxos. If the leader is relatively
// stable, we can skip the promise phase for future instance of the
// protocol with the same leader.
//
// We re-use PromiseResponse to specify the return value of this
// phase. In the case of explicit promise, if a learned action has
// been found in a response, this phase succeeds immediately with the
// 'okay' field set to true and the 'action' field set to the learned
// action. If no learned action has been found in a quorum of
// replicas, we first check if some of them reply Nack (i.e., they
// refuse to give promise). If yes, we set the 'okay' field to false
// and set the 'proposal' field to be the highest proposal number seen
// in these Nack responses. If none of them replies Nack, we set the
// 'okay' field to true and set the 'action' field to be the action
// that is performed by the proposer with the highest proposal number
// in these responses. If no action has been found in these responses,
// we leave the 'action' field unset.
//
// In the case of implicit promise, we must wait until a quorum of
// replicas have replied. If some of them reply Nack, we set the
// 'okay' field to false and set the 'proposal' field to be the
// highest proposal number seen in these Nack responses. If none of
// them replies Nack, we set the 'okay' field to true and set the
// 'position' field to be the highest position (end position) seen in
// these responses.
extern process::Future<PromiseResponse> promise(
    size_t quorum,
    const process::Shared<Network>& network,
    uint64_t proposal,
    const Option<uint64_t>& position = None());


// Runs the write phase (a.k.a., the propose phase) in Paxos. In this
// phase, the proposer broadcasts a write to replicas. This phase
// succeeds if a quorum of replicas accept the write. A proposer
// cannot write if it hasn't gained enough (i.e., a quorum of)
// promises from replicas. We re-use WriteResponse to specify the
// return value of this phase. We must wait until a quorum of replicas
// have replied. If some of them reply Nack, we set the 'okay' field
// to false and set the 'proposal' field to be the highest proposal
// number seen in these Nack responses. If none of them replies Nack,
// we set the 'okay' field to true.
extern process::Future<WriteResponse> write(
    size_t quorum,
    const process::Shared<Network>& network,
    uint64_t proposal,
    const Action& action);


// Runs the learn phase (a.k.a, the commit phase) in Paxos. In fact,
// this phase is not required, but treated as an optimization. In this
// phase, a proposer broadcasts a learned message to replicas,
// indicating that a consensus has already been reached for the given
// log position. No need to wait for responses from replicas. When
// the future is ready, the learned message has been broadcasted.
extern process::Future<Nothing> learn(
    const process::Shared<Network>& network,
    const Action& action);


// Tries to reach consensus for the given log position by running a
// full Paxos round (i.e., promise -> write -> learn). If no value has
// been previously agreed on for the given log position, a NOP will be
// proposed. This function will automatically retry by bumping the
// proposal number if the specified proposal number is found to be not
// high enough. To ensure liveness, it will inject a random delay
// before retrying. A learned action will be returned when the
// operation succeeds.
extern process::Future<Action> fill(
    size_t quorum,
    const process::Shared<Network>& network,
    uint64_t proposal,
    uint64_t position);

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_CONSENSUS_HPP__
