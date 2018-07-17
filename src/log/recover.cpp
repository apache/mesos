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

#include <stdint.h>
#include <stdlib.h>

#include <set>

#include <mesos/type_utils.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

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
//   B1) If a quorum of replicas are found in VOTING status (no matter
//       what status the local replica is in currently), the local
//       replica will be put in RECOVERING status next.
//   B2) If the local replica is in EMPTY status and all replicas are
//       found in either EMPTY status or STARTING status, the local
//       replica will be put in STARTING status next.
//   B3) If the local replica is in STARTING status and all replicas
//       are found in either STARTING status or VOTING status, the
//       local replica will be put in VOTING status next.
//   B4) Otherwise, goto (A).
//
//   (B2 and B3 are used to do the two-phase auto initialization.)
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
      const Shared<Network>& _network,
      const Metadata::Status& _status,
      bool _autoInitialize,
      const Duration& _timeout)
    : ProcessBase(ID::generate("log-recover-protocol")),
      quorum(_quorum),
      network(_network),
      status(_status),
      autoInitialize(_autoInitialize),
      timeout(_timeout),
      terminating(false) {}

  Future<Option<RecoverResponse>> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Register a callback to handle user initiated discard.
    promise.future().onDiscard(defer(self(), &Self::discard));

    start();
  }

private:
  static Future<Option<RecoverResponse>> timedout(
      Future<Option<RecoverResponse>> future,
      const Duration& timeout)
  {
    LOG(INFO) << "Unable to finish the recover protocol in "
              << timeout << ", retrying";

    future.discard();

    // The 'future' will eventually become DISCARDED, at which time we
    // will re-run the recover protocol. We use the boolean flag
    // 'terminating' to distinguish between a user initiated discard
    // and a timeout induced discard.
    return future;
  }

  void discard()
  {
    // TODO(jieyu): The discard logic here is problematic. It is
    // likely that this process never terminates after 'discard' has
    // been called on the returned future. See details in MESOS-5626.
    terminating = true;
    chain.discard();
  }

  void start()
  {
    VLOG(2) << "Starting to wait for enough quorum of replicas before running "
            << "recovery protocol, expected quroum size: " << stringify(quorum);

    // Wait until there are enough (i.e., quorum of) replicas in the
    // network to avoid unnecessary retries.
    chain = network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .then(defer(self(), &Self::broadcast))
      .then(defer(self(), &Self::receive))
      .after(timeout, lambda::bind(&Self::timedout, lambda::_1, timeout))
      .onAny(defer(self(), &Self::finished, lambda::_1));
  }

  Future<Nothing> broadcast()
  {
    VLOG(2) << "Broadcasting recover request to all replicas";

    // Broadcast recover request to all replicas.
    return network->broadcast(protocol::recover, RecoverRequest())
      .then(defer(self(), &Self::broadcasted, lambda::_1));
  }

  Future<Nothing> broadcasted(const set<Future<RecoverResponse>>& _responses)
  {
    VLOG(2) << "Broadcast request completed";

    responses = _responses;

    // Reset the counters.
    responsesReceived.clear();
    lowestBeginPosition = None();
    highestEndPosition = None();

    return Nothing();
  }

  // Returns None if we need to re-run the protocol.
  Future<Option<RecoverResponse>> receive()
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

  Future<Option<RecoverResponse>> received(
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

    // TODO(jieyu): Currently, we simply calculate the size of the
    // cluster from the quorum size. In the future, we may want to
    // allow users to specify the cluster size in case they want to
    // use a non-standard quorum size (e.g., cluster size = 5, quorum
    // size = 4).
    size_t clusterSize = (2 * quorum) - 1;

    if (autoInitialize) {
      // The following code handles the auto-initialization. Our idea
      // is: we allow a replica in EMPTY status to become VOTING
      // immediately if it finds ALL (i.e., 2 * quorum - 1) replicas
      // are in EMPTY status. This is based on the assumption that the
      // only time ALL replicas are in EMPTY status is during start-up
      // This may not be true if we have a catastrophic failure in
      // which all replicas are gone, and that's exactly the reason we
      // allow users to disable auto-initialization.
      //
      // To do auto-initialization, if we use a single phase protocol
      // and allow a replica to directly transit from EMPTY status to
      // VOTING status, we may run into a state where we cannot make
      // progress even if all replicas are in EMPTY status initially.
      // For example, say the quorum size is 2. All replicas are in
      // EMPTY status initially. One replica broadcasts a recover
      // request and becomes VOTING before other replicas start
      // broadcasting recover requests. In that case, no replica can
      // make progress. To solve this problem, we use a two-phase
      // protocol and introduce an intermediate transient status
      // (STARTING) between EMPTY and VOTING status. A replica in
      // EMPTY status can transit to STARTING status if it find all
      // replicas are in either EMPTY or STARTING status. A replica in
      // STARTING status can transit to VOTING status if it finds all
      // replicas are in either STARTING or VOTING status. In that
      // way, in our previous example, all replicas will be in
      // STARTING status before any of them can transit to VOTING
      // status.
      switch (status) {
        case Metadata::EMPTY:
          if ((responsesReceived[Metadata::EMPTY] +
               responsesReceived[Metadata::STARTING]) >= clusterSize) {
            process::discard(responses);

            RecoverResponse result;
            result.set_status(Metadata::STARTING);

            return result;
          }
          break;
        case Metadata::STARTING:
          if ((responsesReceived[Metadata::STARTING] +
               responsesReceived[Metadata::VOTING]) >= clusterSize) {
            process::discard(responses);

            RecoverResponse result;
            result.set_status(Metadata::VOTING);

            return result;
          }
          break;
        default:
          // Ignore all other cases.
          break;
      }
    } else {
      // Since auto initialization is disabled, we print an advisory
      // message to remind the user to initialize the log manually.
      if (responsesReceived[Metadata::EMPTY] >= clusterSize) {
        LOG(WARNING) << "\n"
                     << "----------------------------------------------------\n"
                     << "Replicated log has not been initialized. Did you\n"
                     << "forget to manually initialize the log (i.e.,\n"
                     << "mesos-log initialize --path=<PATH>)? Note that all\n"
                     << "replicas are not initialized and the above command\n"
                     << "needs to be run on each host!\n"
                     << "----------------------------------------------------";
      }
    }

    // Handle the next response.
    return receive();
  }

  void finished(const Future<Option<RecoverResponse>>& future)
  {
    if (future.isDiscarded()) {
      // We use the boolean flag 'terminating' to distinguish between
      // a user initiated discard and a timeout induced discard. In
      // the case of a user initiated discard, the flag 'terminating'
      // will be set to true in 'Self::discard()'.
      if (terminating) {
        promise.discard();
        terminate(self());
      } else {
        VLOG(2) << "Log recovery timed out waiting for responses, retrying";

        start(); // Re-run the recover protocol after timeout.
      }
    } else if (future.isFailed()) {
      promise.fail(future.failure());
      terminate(self());
    } else {
      promise.set(future.get());
      terminate(self());
    }
  }

  const size_t quorum;
  const Shared<Network> network;
  const Metadata::Status status;
  const bool autoInitialize;
  const Duration timeout;

  set<Future<RecoverResponse>> responses;
  hashmap<Metadata::Status, size_t> responsesReceived;
  Option<uint64_t> lowestBeginPosition;
  Option<uint64_t> highestEndPosition;
  Future<Option<RecoverResponse>> chain;
  bool terminating;

  process::Promise<Option<RecoverResponse>> promise;
};


Future<Option<RecoverResponse>> runRecoverProtocol(
    size_t quorum,
    const Shared<Network>& network,
    const Metadata::Status& status,
    bool autoInitialize,
    const Duration& timeout)
{
  RecoverProtocolProcess* process =
    new RecoverProtocolProcess(
        quorum,
        network,
        status,
        autoInitialize,
        timeout);

  Future<Option<RecoverResponse>> future = process->future();
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
// process, indicating the recovery has completed. If all replicas are
// in EMPTY status and auto-initialization is enabled, a two-phase
// protocol will be used to bootstrap the replicated log.
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
//
// 5) Replica A, B and C are all in EMPTY status. Depending on whether
//    auto-initialization is enabled or not, the replicas will behave
//    differently. If auto-initialization is enabled, all replicas
//    will first go into STARTING status. Once *all* replicas have
//    transitioned out of EMPTY status, the replicas will go into
//    VOTING status. If auto-initialization is disabled, all replicas
//    will remain in EMPTY status.
class RecoverProcess : public Process<RecoverProcess>
{
public:
  RecoverProcess(
      size_t _quorum,
      const Owned<Replica>& _replica,
      const Shared<Network>& _network,
      bool _autoInitialize)
    : ProcessBase(ID::generate("log-recover")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      autoInitialize(_autoInitialize) {}

  Future<Owned<Replica>> future() { return promise.future(); }

protected:
  void initialize() override
  {
    LOG(INFO) << "Starting replica recovery";

    // Register a callback to handle user initiated discard.
    promise.future().onDiscard(defer(self(), &Self::discard));

    start();
  }

  void finalize() override
  {
    VLOG(1) << "Recover process terminated";
  }

private:
  void discard()
  {
    chain.discard();
  }

  void start()
  {
    // Check the current status of the local replica and decide if
    // recovery is needed. Recovery is needed only if the local
    // replica is not in VOTING status.
    chain = replica->status()
      .then(defer(self(), &Self::recover, lambda::_1))
      .onAny(defer(self(), &Self::finished, lambda::_1));
  }

  Future<bool> recover(const Metadata::Status& status)
  {
    LOG(INFO) << "Replica is in " << status << " status";

    if (status == Metadata::VOTING) {
      // No need to do recovery.
      return true;
    } else {
      return runRecoverProtocol(quorum, network, status, autoInitialize)
        .then(defer(self(), &Self::_recover, lambda::_1));
    }
  }

  Future<bool> _recover(const Option<RecoverResponse>& result)
  {
    if (result.isNone()) {
      // Re-run the recover protocol.
      return false;
    }

    switch (result->status()) {
      case Metadata::STARTING:
        // This is the auto-initialization case. As mentioned above, we
        // use a two-phase protocol to bootstrap. When the control
        // reaches here, the first phase just ended. We start the second
        // phase by re-running the recover protocol.
        CHECK(autoInitialize);

        return updateReplicaStatus(Metadata::STARTING)
          .then(defer(self(), &Self::recover, Metadata::STARTING));

      case Metadata::VOTING:
        // This is the also the auto-initialization case. When the
        // control reaches here, the second phase just ended.
        CHECK(autoInitialize);

        return updateReplicaStatus(Metadata::VOTING);

      case Metadata::RECOVERING:
        CHECK(result->has_begin() && result->has_end());

        return updateReplicaStatus(Metadata::RECOVERING)
          .then(defer(self(), &Self::catchup, result->begin(), result->end()));

      default:
        return Failure("Unexpected status returned from the recover protocol");
    }
  }

  Future<bool> catchup(uint64_t begin, uint64_t end)
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

  Future<bool> updateReplicaStatus(const Metadata::Status& status)
  {
    LOG(INFO) << "Updating replica status to " << status;

    return replica->update(status)
      .then(defer(self(), &Self::_updateReplicaStatus, lambda::_1, status));
  }

  Future<bool> _updateReplicaStatus(
      bool updated, const Metadata::Status& status)
  {
    if (!updated) {
      return Failure("Failed to update replica status");
    }

    if (status == Metadata::VOTING) {
      LOG(INFO) << "Successfully joined the Paxos group";
    }

    return true;
  }

  Future<bool> getReplicaOwnership(Shared<Replica> shared)
  {
    // Try to re-gain the ownership of the replica.
    return shared.own()
      .then(defer(self(), &Self::_getReplicaOwnership, lambda::_1));
  }

  Future<bool> _getReplicaOwnership(Owned<Replica> owned)
  {
    replica = owned;

    return true;
  }

  void finished(const Future<bool>& future)
  {
    if (future.isDiscarded()) {
      promise.discard();
      terminate(self());
    } else if (future.isFailed()) {
      promise.fail(future.failure());
      terminate(self());
    } else if (!future.get()) {
      // We add a random delay before each retry because we do not
      // want to saturate the network/disk IO in some cases. The delay
      // is chosen randomly to reduce the likelyhood of conflicts
      // (i.e., a replica receives a recover request while it is
      // changing its status).
      static const Duration T = Milliseconds(500);
      Duration d = T * (1.0 + (double) os::random() / RAND_MAX);
      VLOG(2) << "Retrying recovery in " << stringify(d);
      delay(d, self(), &Self::start);
    } else {
      promise.set(replica);
      terminate(self());
    }
  }

  const size_t quorum;
  Owned<Replica> replica;
  const Shared<Network> network;
  const bool autoInitialize;

  // The value in this future speficies if the recovery was
  // successfull or we need to retry it.
  Future<bool> chain;

  process::Promise<Owned<Replica>> promise;
};


Future<Owned<Replica>> recover(
    size_t quorum,
    const Owned<Replica>& replica,
    const Shared<Network>& network,
    bool autoInitialize)
{
  RecoverProcess* process =
    new RecoverProcess(
        quorum,
        replica,
        network,
        autoInitialize);

  Future<Owned<Replica>> future = process->future();
  spawn(process, true);
  return future;
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
