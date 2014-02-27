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

#include <stdint.h>
#include <stdlib.h>

#include <set>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

#include "common/type_utils.hpp"

#include "log/catchup.hpp"
#include "log/recover.hpp"

#include "messages/log.hpp"

using namespace process;

using std::set;

namespace mesos {
namespace internal {
namespace log {


// This class is responsible for executing the log recover protocol.
// Any time a replica in non-VOTING status starts, we will run this
// protocol. We first broadcast a recover request to all the replicas
// in the network, and then collect recover responses to decide what
// status the local replica should be in next. The details of the
// recover protocol is shown as follows:
//
// A) Broadcast a RecoverRequest to all replicas in the network.
// B) Collect RecoverResponse from each replica
//   B1) If a quorum of replicas are found in VOTING status, the local
//       replica will be in RECOVERING status next.
//   B2) Otherwise, goto (A).
//
// We re-use RecoverResponse to specify the return value. The 'status'
// field specifies the next status of the local replica. If the next
// status is RECOVERING, we set the fields 'begin' and 'end' to be the
// lowest begin and highest end position seen in these responses.
class RecoverProtocolProcess : public Process<RecoverProtocolProcess>
{
public:
  RecoverProtocolProcess(
      size_t _quorum,
      const Shared<Network>& _network)
    : ProcessBase(ID::generate("log-recover-protocol")),
      quorum(_quorum),
      network(_network) {}

  Future<RecoverResponse> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Register a callback to handle user initiated discard.
    promise.future().onDiscard(defer(self(), &Self::discard));

    start();
  }

private:
  void discard()
  {
    chain.discard();
  }

  void start()
  {
    // Wait until there are enough (i.e., quorum of) replicas in the
    // network to avoid unnecessary retries.
    chain = network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .then(defer(self(), &Self::broadcast))
      .then(defer(self(), &Self::receive))
      .onAny(defer(self(), &Self::finished, lambda::_1));
  }

  Future<Nothing> broadcast()
  {
    // Broadcast recover request to all replicas.
    return network->broadcast(protocol::recover, RecoverRequest())
      .then(defer(self(), &Self::broadcasted, lambda::_1));
  }

  Future<Nothing> broadcasted(const set<Future<RecoverResponse> >& _responses)
  {
    responses = _responses;

    // Reset the counters.
    responsesReceived.clear();
    lowestBeginPosition = None();
    highestEndPosition = None();

    return Nothing();
  }

  // Returns None if we need to re-run the protocol.
  Future<Option<RecoverResponse> > receive()
  {
    if (responses.empty()) {
      // All responses have been received but we haven't received
      // enough (i.e., a quorum of) responses from VOTING replicas to
      // start the catch-up. We will re-run the recovery protocol.
      return None();
    }

    // Instead of using a for loop here, we use select to process
    // responses one after another so that we can ignore the rest if
    // we have collected enough responses.
    return select(responses)
      .then(defer(self(), &Self::received, lambda::_1));
  }

  Future<Option<RecoverResponse> > received(
      const Future<RecoverResponse>& future)
  {
    // Enforced by the select semantics.
    CHECK_READY(future);

    // Remove this future from 'responses' so that we do not listen on
    // it the next time we invoke select.
    responses.erase(future);

    const RecoverResponse& response = future.get();

    LOG(INFO) << "Received a recover response from a replica in "
              << response.status() << " status";

    responsesReceived[response.status()]++;

    // We need to remember the lowest begin position and highest end
    // position seen from VOTING replicas.
    if (response.status() == Metadata::VOTING) {
      CHECK(response.has_begin() && response.has_end());

      lowestBeginPosition = min(lowestBeginPosition, response.begin());
      highestEndPosition = max(highestEndPosition, response.end());
    }

    // If we got responses from a quorum of VOTING replicas, the local
    // replica will be put in RECOVERING status and start catching up.
    // It is likely that the local replica is in RECOVERING status
    // already. This is the case where the replica crashes during
    // catch-up. When it restarts, we need to recalculate the lowest
    // begin position and the highest end position since we haven't
    // persisted this information on disk.
    if (responsesReceived[Metadata::VOTING] >= quorum) {
      process::discard(responses);

      CHECK_SOME(lowestBeginPosition);
      CHECK_SOME(highestEndPosition);
      CHECK_LE(lowestBeginPosition.get(), highestEndPosition.get());

      RecoverResponse result;
      result.set_status(Metadata::RECOVERING);
      result.set_begin(lowestBeginPosition.get());
      result.set_end(highestEndPosition.get());

      return result;
    }

    // Handle the next response.
    return receive();
  }

  void finished(const Future<Option<RecoverResponse> >& future)
  {
    if (future.isDiscarded()) {
      promise.discard();
      terminate(self());
    } else if (future.isFailed()) {
      promise.fail(future.failure());
      terminate(self());
    } else if (future.get().isNone()) {
      // Re-run the protocol. We add a random delay before each retry
      // because we do not want to saturate the network/disk IO in
      // some cases. The delay is chosen randomly to reduce the
      // likelihood of conflicts (i.e., a replica receives a recover
      // request while it is changing its status).
      static const Duration T = Milliseconds(500);
      Duration d = T * (1.0 + (double) ::random() / RAND_MAX);
      delay(d, self(), &Self::start);
    } else {
      promise.set(future.get().get());
      terminate(self());
    }
  }

  const size_t quorum;
  const Shared<Network> network;

  set<Future<RecoverResponse> > responses;
  hashmap<Metadata::Status, size_t> responsesReceived;
  Option<uint64_t> lowestBeginPosition;
  Option<uint64_t> highestEndPosition;
  Future<Option<RecoverResponse> > chain;

  process::Promise<RecoverResponse> promise;
};


// The wrapper for running the recover protocol.
static Future<RecoverResponse> runRecoverProtocol(
    size_t quorum,
    const Shared<Network>& network)
{
  RecoverProtocolProcess* process =
    new RecoverProtocolProcess(quorum, network);

  Future<RecoverResponse> future = process->future();
  spawn(process, true);
  return future;
}


// This process is used to recover a replica. We first check the
// status of the local replica. If it is in VOTING status, the recover
// process will terminate immediately. If the local replica is in
// non-VOTING status, we will run the log recover protocol described
// above to decide what status the local replica should be in next. If
// the next status is determined to be RECOVERING, we will start doing
// catch-up. Later, if the local replica has caught-up, we will set
// the status of the local replica to VOTING and terminate the
// process, indicating the recovery has completed.
//
// Here, we list a few scenarios and show how the recover process will
// respond in those scenarios. All the examples assume a quorum size
// of 2. Remember that a new replica is always put in EMPTY status
// initially.
//
// 1) Replica A, B and C are all in VOTING status. The operator adds
//    replica D. In that case, D will go into RECOVERING status and
//    then go into VOTING status. Therefore, we should avoid adding a
//    new replica unless we know that one replica has been removed.
//
// 2) Replica A and B are in VOTING status. The operator adds replica
//    C. In that case, C will go into RECOVERING status and then go
//    into VOTING status, which is expected.
//
// 3) Replica A is in VOTING status. The operator adds replica B. In
//    that case, B will stay in EMPTY status forever. This is expected
//    because we cannot make progress if VOTING replicas are not
//    enough (i.e., less than quorum).
//
// 4) Replica A is in VOTING status and B is in EMPTY status. The
//    operator adds replica C. In that case, C will stay in EMPTY
//    status forever similar to case 3).
class RecoverProcess : public Process<RecoverProcess>
{
public:
  RecoverProcess(
      size_t _quorum,
      const Owned<Replica>& _replica,
      const Shared<Network>& _network)
    : ProcessBase(ID::generate("log-recover")),
      quorum(_quorum),
      replica(_replica),
      network(_network) {}

  Future<Owned<Replica> > future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    LOG(INFO) << "Starting replica recovery";

    // Register a callback to handle user initiated discard.
    promise.future().onDiscard(defer(self(), &Self::discard));

    // Check the current status of the local replica and decide if
    // recovery is needed. Recovery is needed only if the local
    // replica is not in VOTING status.
    chain = replica->status()
      .then(defer(self(), &Self::recover, lambda::_1))
      .onAny(defer(self(), &Self::finished, lambda::_1));
  }

  virtual void finalize()
  {
    LOG(INFO) << "Recover process terminated";
  }

private:
  void discard()
  {
    chain.discard();
  }

  Future<Nothing> recover(const Metadata::Status& status)
  {
    LOG(INFO) << "Replica is in " << status << " status";

    if (status == Metadata::VOTING) {
      // No need to do recovery.
      return Nothing();
    } else {
      return runRecoverProtocol(quorum, network)
        .then(defer(self(), &Self::_recover, lambda::_1));
    }
  }

  Future<Nothing> _recover(const RecoverResponse& result)
  {
    if (result.status() == Metadata::RECOVERING) {
      CHECK(result.has_begin() && result.has_end());

      return updateReplicaStatus(Metadata::RECOVERING)
        .then(defer(self(), &Self::catchup, result.begin(), result.end()));
    } else {
      return Failure("Unexpected status returned from the recover protocol");
    }
  }

  Future<Nothing> catchup(uint64_t begin, uint64_t end)
  {
    // We reach here either because the log is empty (uninitialized),
    // or the log is not empty but a previous unfinished catch-up
    // attempt has been detected (the process crashes/killed when
    // catching up). In either case, the local replica may have lost
    // some data and Paxos states, and should not be allowed to vote.
    // Otherwise, we may introduce inconsistency in the log as the
    // local replica could have accepted a write which it would not
    // have accepted if the data and the Paxos states were not lost.
    // Now, the question is how many positions the local replica
    // should catch up before it can be allowed to vote. We find that
    // it is sufficient to catch-up positions from _begin_ to _end_
    // where _begin_ is the smallest position seen in a quorum of
    // VOTING replicas and _end_ is the largest position seen in a
    // quorum of VOTING replicas. Here is the correctness argument.
    // For a position _e_ larger than _end_, obviously no value has
    // been agreed on for that position. Otherwise, we should find at
    // least one VOTING replica in a quorum of replicas such that its
    // end position is larger than _end_. For the same reason, a
    // coordinator should not have collected enough promises for
    // position _e_. Therefore, it's safe for the local replica to
    // vote for that position. For a position _b_ smaller than
    // _begin_, it should have already been truncated and the
    // truncation should have already been agreed. Therefore, allowing
    // the local replica to vote for that position is safe.
    CHECK_LE(begin, end);

    LOG(INFO) << "Starting catch-up from position " << begin << " to " << end;

    IntervalSet<uint64_t> positions(
        Bound<uint64_t>::closed(begin),
        Bound<uint64_t>::closed(end));

    // Share the ownership of the replica. From this point until the
    // point where the ownership of the replica is regained, we should
    // not access the 'replica' field.
    Shared<Replica> shared = replica.share();

    // Since we do not know what proposal number to use (the log is
    // empty), we use none and leave log::catchup to automatically
    // bump the proposal number.
    return log::catchup(quorum, shared, network, None(), positions)
      .then(defer(self(), &Self::getReplicaOwnership, shared))
      .then(defer(self(), &Self::updateReplicaStatus, Metadata::VOTING));
  }

  Future<Nothing> updateReplicaStatus(const Metadata::Status& status)
  {
    LOG(INFO) << "Updating replica status to " << status;

    return replica->update(status)
      .then(defer(self(), &Self::_updateReplicaStatus, lambda::_1, status));
  }

  Future<Nothing> _updateReplicaStatus(
      bool updated, const Metadata::Status& status)
  {
    if (!updated) {
      return Failure("Failed to update replica status");
    }

    if (status == Metadata::VOTING) {
      LOG(INFO) << "Successfully joined the Paxos group";
    }

    return Nothing();
  }

  Future<Nothing> getReplicaOwnership(Shared<Replica> shared)
  {
    // Try to regain the ownership of the replica.
    return shared.own()
      .then(defer(self(), &Self::_getReplicaOwnership, lambda::_1));
  }

  Future<Nothing> _getReplicaOwnership(Owned<Replica> owned)
  {
    replica = owned;

    return Nothing();
  }

  void finished(const Future<Nothing>& future)
  {
    if (future.isDiscarded()) {
      promise.discard();
      terminate(self());
    } else if (future.isFailed()) {
      promise.fail(future.failure());
      terminate(self());
    } else {
      promise.set(replica);
      terminate(self());
    }
  }

  const size_t quorum;
  Owned<Replica> replica;
  const Shared<Network> network;

  Future<Nothing> chain;

  process::Promise<Owned<Replica> > promise;
};


Future<Owned<Replica> > recover(
    size_t quorum,
    const Owned<Replica>& replica,
    const Shared<Network>& network)
{
  RecoverProcess* process = new RecoverProcess(quorum, replica, network);
  Future<Owned<Replica> > future = process->future();
  spawn(process, true);
  return future;
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
