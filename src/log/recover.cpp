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

#include <stdlib.h>

#include <set>
#include <vector>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

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
using std::vector;

namespace mesos {
namespace internal {
namespace log {

// This process is used to recover a replica. The flow of the recover
// process is described as follows:
// A) Check the status of the local replica.
//    A1) If it is VOTING, exit.
//    A2) If it is not VOTING, goto (B).
// B) Broadcast a RecoverRequest to all replicas in the network.
//    B1) <<< Catch-up >>> If a quorum of replicas are found in VOTING
//        status (no matter what the status of the local replica is),
//        set the status of the local replica to RECOVERING, and start
//        doing catch-up. If the local replica has been caught-up, set
//        the status of the local replica to VOTING and exit.
//    B2) If a quorum is not found, goto (B).
//
// In the following, we list a few scenarios and show how the recover
// process will respond in those scenarios. All the examples assume a
// quorum size of 2. Remember that a new replica is always put in
// EMPTY status initially.
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
    LOG(INFO) << "Start recovering a replica";

    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
          static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Check the current status of the local replica and decide if
    // recovery is needed. Recovery is needed if the local replica is
    // not in VOTING status.
    replica->status().onAny(defer(self(), &Self::checked, lambda::_1));
  }

  virtual void finalize()
  {
    LOG(INFO) << "Recover process terminated";

    // Cancel all operations if they are still pending.
    discard(responses);
    catching.discard();
  }

private:
  void checked(const Future<Metadata::Status>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to get replica status: " + future.failure() :
          "Not expecting discarded future");

      terminate(self());
      return;
    }

    status = future.get();

    LOG(INFO) << "Replica is in " << status << " status";

    if (status == Metadata::VOTING) {
      promise.set(replica);
      terminate(self());
    } else {
      recover();
    }
  }

  void recover()
  {
    CHECK_NE(status, Metadata::VOTING);

    // Wait until there are enough (i.e., quorum of) replicas in the
    // network to avoid unnecessary retries.
    network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }

  void watched(const Future<size_t>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          future.failure() :
          "Not expecting discarded future");

      terminate(self());
      return;
    }

    CHECK_GE(future.get(), quorum);

    // Broadcast recover request to all replicas.
    network->broadcast(protocol::recover, RecoverRequest())
      .onAny(defer(self(), &Self::broadcasted, lambda::_1));
  }

  void broadcasted(const Future<set<Future<RecoverResponse> > >& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to broadcast the recover request: " + future.failure() :
          "Not expecting discarded future");

      terminate(self());
      return;
    }

    responses = future.get();

    if (responses.empty()) {
      // Retry if no replica is currently in the network.
      retry();
    } else {
      // Instead of using a for loop here, we use select to process
      // responses one after another so that we can ignore the rest if
      // we have collected enough responses.
      select(responses)
        .onReady(defer(self(), &Self::received, lambda::_1));

      // Reset the counters.
      responsesReceived.clear();
      lowestBeginPosition = None();
      highestEndPosition = None();
    }
  }

  void received(const Future<RecoverResponse>& future)
  {
    // Enforced by the select semantics.
    CHECK(future.isReady());

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
      discard(responses);
      update(Metadata::RECOVERING);
      return;
    }

    if (responses.empty()) {
      // All responses have been received but neither have we received
      // enough responses from VOTING replicas to do catch-up, nor are
      // we in start-up case. This is either because we don't have
      // enough replicas in the network (e.g. ZooKeeper blip), or we
      // don't have enough VOTING replicas to proceed. We will retry
      // the recovery in both cases.
      retry();
    } else {
      // Wait for the next response.
      select(responses)
        .onReady(defer(self(), &Self::received, lambda::_1));
    }
  }

  void update(const Metadata::Status& _status)
  {
    LOG(INFO) << "Updating replica status from "
              << status << " to " << _status;

    replica->update(_status)
      .onAny(defer(self(), &Self::updated, _status, lambda::_1));
  }

  void updated(const Metadata::Status& _status, const Future<bool>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to update replica status: " + future.failure() :
          "Not expecting discarded future");

      terminate(self());
      return;
    } else if (!future.get()) {
      promise.fail("Failed to update replica status");
      terminate(self());
      return;
    }

    // The replica status has been updated successfully. Depending on
    // the new status, we decide what the next action should be.
    status = _status;

    if (status == Metadata::VOTING) {
      LOG(INFO) << "Successfully joined the Paxos group";

      promise.set(replica);
      terminate(self());
    } else if (status == Metadata::RECOVERING) {
      catchup();
    } else {
      // The replica should not be in any other status.
      LOG(FATAL) << "Unexpected replica status";
    }
  }

  void catchup()
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
    CHECK(lowestBeginPosition.isSome());
    CHECK(highestEndPosition.isSome());
    CHECK_LE(lowestBeginPosition.get(), highestEndPosition.get());

    uint64_t begin = lowestBeginPosition.get();
    uint64_t end = highestEndPosition.get();

    LOG(INFO) << "Starting catch-up from position "
              << lowestBeginPosition.get() << " to "
              << highestEndPosition.get();

    vector<uint64_t> positions;
    for (uint64_t p = begin; p <= end; ++p) {
      positions.push_back(p);
    }

    // Share the ownership of the replica. From this point until the
    // point where the ownership of the replica is regained, we should
    // not access the 'replica' field.
    Shared<Replica> shared = replica.share();

    // Since we do not know what proposal number to use (the log is
    // empty), we use none and leave log::catchup to automatically
    // bump the proposal number.
    catching = log::catchup(quorum, shared, network, None(), positions);
    catching.onAny(defer(self(), &Self::caughtup, shared, lambda::_1));
  }

  void caughtup(Shared<Replica> shared, const Future<Nothing>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to catch-up: " + future.failure() :
          "Not expecting discarded future");

      terminate(self());
    } else {
      // Try to regain the ownership of the replica.
      shared.own().onAny(defer(self(), &Self::owned, lambda::_1));
    }
  }

  void owned(const Future<Owned<Replica> >& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to own the replica: " + future.failure() :
          "Not expecting discarded future");

      terminate(self());
    } else {
      // Allow the replica to vote once the catch-up is done.
      replica = future.get();
      update(Metadata::VOTING);
    }
  }

  void retry()
  {
    // We add a random delay before each retry because we do not want
    // to saturate the network/disk IO in some cases (e.g., network
    // size is less than quorum). The delay is chosen randomly to
    // reduce the likelihood of conflicts (i.e., a replica receives a
    // recover request while it is changing its status).
    static const Duration T = Milliseconds(500);
    Duration d = T * (1.0 + (double) ::random() / RAND_MAX);
    delay(d, self(), &Self::recover);
  }

  const size_t quorum;
  Owned<Replica> replica;
  const Shared<Network> network;

  Metadata::Status status;
  set<Future<RecoverResponse> > responses;
  hashmap<Metadata::Status, size_t> responsesReceived;
  Option<uint64_t> lowestBeginPosition;
  Option<uint64_t> highestEndPosition;
  Future<Nothing> catching;

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
