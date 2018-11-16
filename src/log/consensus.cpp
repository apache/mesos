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

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/nothing.hpp>
#include <stout/foreach.hpp>

#include "log/consensus.hpp"
#include "log/replica.hpp"

using namespace process;

using std::set;

namespace mesos {
namespace internal {
namespace log {

static bool isRejectedPromise(const PromiseResponse& response)
{
  if (response.has_type()) {
    // New format (Mesos >= 0.26).
    return response.type() == PromiseResponse::REJECT;
  } else {
    // Old format (Mesos < 0.26).
    return !response.okay();
  }
}


static bool isRejectedWrite(const WriteResponse& response)
{
  if (response.has_type()) {
    // New format (Mesos >= 0.26).
    return response.type() == WriteResponse::REJECT;
  } else {
    // Old format (Mesos < 0.26).
    return !response.okay();
  }
}


class ExplicitPromiseProcess : public Process<ExplicitPromiseProcess>
{
public:
  ExplicitPromiseProcess(
      size_t _quorum,
      const Shared<Network>& _network,
      uint64_t _proposal,
      uint64_t _position)
    : ProcessBase(ID::generate("log-explicit-promise")),
      quorum(_quorum),
      network(_network),
      proposal(_proposal),
      position(_position),
      responsesReceived(0),
      ignoresReceived(0) {}

  ~ExplicitPromiseProcess() override {}

  Future<PromiseResponse> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Wait until there are enough (i.e., quorum of) replicas in the
    // network. This is because if there are less than quorum number
    // of replicas in the network, the operation will not finish.
    network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }

  void finalize() override
  {
    // This process will be terminated when we get responses from a
    // quorum of replicas. In that case, we no longer care about
    // responses from other replicas, thus discarding them here.
    discard(responses);

    promise.discard();
  }

private:
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

    request.set_proposal(proposal);
    request.set_position(position);

    network->broadcast(protocol::promise, request)
      .onAny(defer(self(), &Self::broadcasted, lambda::_1));
  }

  void broadcasted(const Future<set<Future<PromiseResponse>>>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to broadcast explicit promise request: " + future.failure() :
          "Not expecting discarded future");
      terminate(self());
      return;
    }

    responses = future.get();
    foreach (const Future<PromiseResponse>& response, responses) {
      response.onReady(defer(self(), &Self::received, lambda::_1));
    }
  }

  void received(const PromiseResponse& response)
  {
    if (response.has_type() && response.type() ==
        PromiseResponse::IGNORED) {
      ignoresReceived++;

      // A quorum of replicas have ignored the request.
      if (ignoresReceived >= quorum) {
        LOG(INFO) << "Aborting explicit promise request because "
                  << ignoresReceived << " ignores received";

        // If the "type" is PromiseResponse::IGNORED, the rest of the
        // fields don't matter.
        PromiseResponse result;
        result.set_type(PromiseResponse::IGNORED);

        promise.set(result);
        terminate(self());
      }

      return;
    }

    responsesReceived++;

    if (isRejectedPromise(response)) {
      // Failed to get the promise from a replica for this position
      // because it has been promised to a proposer with a higher
      // proposal number. The 'proposal' field in the response
      // specifies the proposal number. It is found to be larger than
      // the proposal number used in this phase.
      if (highestNackProposal.isNone() ||
          highestNackProposal.get() < response.proposal()) {
        highestNackProposal = response.proposal();
      }
    } else if (highestNackProposal.isSome()) {
      // We still want to wait for more potential NACK responses so we
      // can return the highest proposal number seen but we don't care
      // about any more ACK responses.
    } else {
      // The position has been promised to us so the 'proposal' field
      // should match the proposal we sent in the request.
      CHECK_EQ(response.proposal(), request.proposal());

      if (response.has_action()) {
        CHECK_EQ(response.action().position(), position);
        if (response.action().has_learned() && response.action().learned()) {
          // Received a learned action. Note that there is no checking
          // that we get the _same_ learned action in the event we get
          // multiple responses with learned actions, we just take the
          // "first". In fact, there is a specific instance in which
          // learned actions will NOT be the same! In this instance,
          // one replica may return that the action is a learned no-op
          // because it knows the position has been truncated while
          // another replica (that hasn't learned the truncation yet)
          // might return the actual action at this position. Picking
          // either action is _correct_, since eventually we know this
          // position will be truncated. Fun!
          // TODO(neilc): Create a test case for this scenario.
          promise.set(response);

          // The remaining responses will be discarded in 'finalize'.
          terminate(self());
          return;
        } else if (response.action().has_performed()) {
          // An action has already been performed in this position, we
          // need to save the action with the highest proposal number.
          if (highestAckAction.isNone() ||
              (highestAckAction->performed() < response.action().performed())) {
            highestAckAction = response.action();
          }
        } else {
          // Received a response for a position that had previously
          // been promised to some other proposer but an action had
          // not been performed or learned. The position is now
          // promised to us. No need to do anything here.
        }
      } else {
        // Received a response without an action associated with it.
        // This is the case when this proposer is the first to request
        // a promise for this log position.
        CHECK(response.has_position());
        CHECK_EQ(response.position(), position);
      }
    }

    if (responsesReceived >= quorum) {
      // A quorum of replicas have replied.
      PromiseResponse result;

      if (highestNackProposal.isSome()) {
        result.set_type(PromiseResponse::REJECT);
        result.set_okay(false);
        result.set_proposal(highestNackProposal.get());
      } else {
        result.set_type(PromiseResponse::ACCEPT);
        result.set_okay(true);
        if (highestAckAction.isSome()) {
          result.mutable_action()->CopyFrom(highestAckAction.get());
        }
      }

      promise.set(result);
      terminate(self());
    }
  }

  const size_t quorum;
  const Shared<Network> network;
  const uint64_t proposal;
  const uint64_t position;

  PromiseRequest request;
  set<Future<PromiseResponse>> responses;
  size_t responsesReceived;
  size_t ignoresReceived;
  Option<uint64_t> highestNackProposal;
  Option<Action> highestAckAction;

  process::Promise<PromiseResponse> promise;
};


class ImplicitPromiseProcess : public Process<ImplicitPromiseProcess>
{
public:
  ImplicitPromiseProcess(
      size_t _quorum,
      const Shared<Network>& _network,
      uint64_t _proposal)
    : ProcessBase(ID::generate("log-implicit-promise")),
      quorum(_quorum),
      network(_network),
      proposal(_proposal),
      responsesReceived(0),
      ignoresReceived(0) {}

  ~ImplicitPromiseProcess() override {}

  Future<PromiseResponse> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Wait until there are enough (i.e., quorum of) replicas in the
    // network. This is because if there are less than quorum number
    // of replicas in the network, the operation will not finish.
    network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }

  void finalize() override
  {
    // This process will be terminated when we get responses from a
    // quorum of replicas. In that case, we no longer care about
    // responses from other replicas, thus discarding them here.
    discard(responses);

    promise.discard();
  }

private:
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

    request.set_proposal(proposal);

    network->broadcast(protocol::promise, request)
      .onAny(defer(self(), &Self::broadcasted, lambda::_1));
  }

  void broadcasted(const Future<set<Future<PromiseResponse>>>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to broadcast implicit promise request: " + future.failure() :
          "Not expecting discarded future");
      terminate(self());
      return;
    }

    responses = future.get();
    foreach (const Future<PromiseResponse>& response, responses) {
      response.onReady(defer(self(), &Self::received, lambda::_1));
    }
  }

  void received(const PromiseResponse& response)
  {
    if (response.has_type() && response.type() ==
        PromiseResponse::IGNORED) {
      ignoresReceived++;

      // A quorum of replicas have ignored the request.
      if (ignoresReceived >= quorum) {
        LOG(INFO) << "Aborting implicit promise request because "
                  << ignoresReceived << " ignores received";

        // If the "type" is PromiseResponse::IGNORED, the rest of the
        // fields don't matter.
        PromiseResponse result;
        result.set_type(PromiseResponse::IGNORED);

        promise.set(result);
        terminate(self());
      }

      return;
    }

    responsesReceived++;

    if (isRejectedPromise(response)) {
      // Failed to get the promise from a replica because it has
      // promised a proposer with a higher proposal number. The
      // 'proposal' field in the response specifies the proposal
      // number. It is found to be larger than the proposal number
      // used in this phase.
      if (highestNackProposal.isNone() ||
          highestNackProposal.get() < response.proposal()) {
        highestNackProposal = response.proposal();
      }
    } else if (highestNackProposal.isSome()) {
      // We still want to wait for more potential NACK responses so we
      // can return the highest proposal number seen but we don't care
      // about any more ACK responses.
    } else {
      CHECK(response.has_position());
      if (highestEndPosition.isNone() ||
          highestEndPosition.get() < response.position()) {
        highestEndPosition = response.position();
      }
    }

    if (responsesReceived >= quorum) {
      // A quorum of replicas have replied.
      PromiseResponse result;

      if (highestNackProposal.isSome()) {
        result.set_type(PromiseResponse::REJECT);
        result.set_okay(false);
        result.set_proposal(highestNackProposal.get());
      } else {
        CHECK_SOME(highestEndPosition);

        result.set_type(PromiseResponse::ACCEPT);
        result.set_okay(true);
        result.set_position(highestEndPosition.get());
      }

      promise.set(result);
      terminate(self());
    }
  }

  const size_t quorum;
  const Shared<Network> network;
  const uint64_t proposal;

  PromiseRequest request;
  set<Future<PromiseResponse>> responses;
  size_t responsesReceived;
  size_t ignoresReceived;
  Option<uint64_t> highestNackProposal;
  Option<uint64_t> highestEndPosition;

  process::Promise<PromiseResponse> promise;
};


class WriteProcess : public Process<WriteProcess>
{
public:
  WriteProcess(
      size_t _quorum,
      const Shared<Network>& _network,
      uint64_t _proposal,
      const Action& _action)
    : ProcessBase(ID::generate("log-write")),
      quorum(_quorum),
      network(_network),
      proposal(_proposal),
      action(_action),
      responsesReceived(0),
      ignoresReceived(0) {}

  ~WriteProcess() override {}

  Future<WriteResponse> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Wait until there are enough (i.e., quorum of) replicas in the
    // network. This is because if there are less than quorum number
    // of replicas in the network, the operation will not finish.
    network->watch(quorum, Network::GREATER_THAN_OR_EQUAL_TO)
      .onAny(defer(self(), &Self::watched, lambda::_1));
  }

  void finalize() override
  {
    // This process will be terminated when we get responses from a
    // quorum of replicas. In that case, we no longer care about
    // responses from other replicas, thus discarding them here.
    discard(responses);

    promise.discard();
  }

private:
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

    request.set_proposal(proposal);
    request.set_position(action.position());
    request.set_type(action.type());
    switch (action.type()) {
      case Action::NOP:
        CHECK(action.has_nop());
        request.mutable_nop();
        break;
      case Action::APPEND:
        CHECK(action.has_append());
        request.mutable_append()->CopyFrom(action.append());
        break;
      case Action::TRUNCATE:
        CHECK(action.has_truncate());
        request.mutable_truncate()->CopyFrom(action.truncate());
        break;
      default:
        LOG(FATAL) << "Unknown Action::Type " << action.type();
    }

    network->broadcast(protocol::write, request)
      .onAny(defer(self(), &Self::broadcasted, lambda::_1));
  }

  void broadcasted(const Future<set<Future<WriteResponse>>>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Failed to broadcast the write request: " + future.failure() :
          "Not expecting discarded future");
      terminate(self());
      return;
    }

    responses = future.get();
    foreach (const Future<WriteResponse>& response, responses) {
      response.onReady(defer(self(), &Self::received, lambda::_1));
    }
  }

  void received(const WriteResponse& response)
  {
    CHECK_EQ(response.position(), request.position());

    if (response.has_type() && response.type() ==
        WriteResponse::IGNORED) {
      ignoresReceived++;

      if (ignoresReceived >= quorum) {
        LOG(INFO) << "Aborting write request because "
                  << ignoresReceived << " ignores received";

        // If the "type" is WriteResponse::IGNORED, the rest of the
        // fields don't matter.
        WriteResponse result;
        result.set_type(WriteResponse::IGNORED);

        promise.set(result);
        terminate(self());
      }

      return;
    }

    responsesReceived++;

    if (isRejectedWrite(response)) {
      // A replica rejects the write request because this position has
      // been promised to a proposer with a higher proposal number.
      // The 'proposal' field in the response specifies the proposal
      // number. It is found to be larger than the proposal number
      // used in this phase.
      if (highestNackProposal.isNone() ||
          highestNackProposal.get() < response.proposal()) {
        highestNackProposal = response.proposal();
      }
    }

    if (responsesReceived >= quorum) {
      // A quorum of replicas have replied.
      WriteResponse result;

      if (highestNackProposal.isSome()) {
        result.set_type(WriteResponse::REJECT);
        result.set_okay(false);
        result.set_proposal(highestNackProposal.get());
      } else {
        result.set_type(WriteResponse::ACCEPT);
        result.set_okay(true);
      }

      promise.set(result);
      terminate(self());
    }
  }

  const size_t quorum;
  const Shared<Network> network;
  const uint64_t proposal;
  const Action action;

  WriteRequest request;
  set<Future<WriteResponse>> responses;
  size_t responsesReceived;
  size_t ignoresReceived;
  Option<uint64_t> highestNackProposal;

  process::Promise<WriteResponse> promise;
};


class FillProcess : public Process<FillProcess>
{
public:
  FillProcess(
      size_t _quorum,
      const Shared<Network>& _network,
      uint64_t _proposal,
      uint64_t _position)
    : ProcessBase(ID::generate("log-fill")),
      quorum(_quorum),
      network(_network),
      position(_position),
      proposal(_proposal) {}

  ~FillProcess() override {}

  Future<Action> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    runPromisePhase();
  }

  void finalize() override
  {
    // Discard the futures we're waiting for.
    promising.discard();
    writing.discard();

    // TODO(benh): Discard our promise only after 'promising' and
    // 'writing' have completed (ready, failed, or discarded).
    promise.discard();
  }

private:
  void runPromisePhase()
  {
    promising = log::promise(quorum, network, proposal, position);
    promising.onAny(defer(self(), &Self::checkPromisePhase));
  }

  void checkPromisePhase()
  {
    // The future 'promising' can only be discarded in 'finalize'
    CHECK(!promising.isDiscarded());

    if (promising.isFailed()) {
      promise.fail("Explicit promise phase failed: " + promising.failure());
      terminate(self());
    } else {
      const PromiseResponse& response = promising.get();
      if (!response.okay()) {
        // Retry with a higher proposal number.
        retry(response.proposal());
      } else if (response.has_action()) {
        // A previously performed write has been found. Paxos
        // restricts us to write the same value.
        Action action = response.action();

        CHECK_EQ(action.position(), position);
        CHECK(action.has_type());
        action.set_promised(proposal);
        action.set_performed(proposal);

        if (action.has_learned() && action.learned()) {
          // If the promise phase returns a learned action, we simply
          // learn the action by broadcasting a learned message. We
          // don't check if a quorum of replicas acknowledge the
          // learned message. Because of that, a catch-up replica
          // needs to make sure that all positions it needs to recover
          // have been learned before it can re-join the Paxos (i.e.,
          // invoking log::catchup). Otherwise, we may not have a
          // quorum of replicas remember an agreed value, leading to
          // potential inconsistency in the log.
          runLearnPhase(action);
        } else {
          runWritePhase(action);
        }
      } else {
        // No previously performed write has been found. We can
        // write any value. We choose to write a NOP.
        Action action;
        action.set_position(position);
        action.set_promised(proposal);
        action.set_performed(proposal);
        action.set_type(Action::NOP);
        action.mutable_nop();

        runWritePhase(action);
      }
    }
  }

  void runWritePhase(const Action& action)
  {
    CHECK(!action.has_learned() || !action.learned());

    writing = log::write(quorum, network, proposal, action);
    writing.onAny(defer(self(), &Self::checkWritePhase, action));
  }

  void checkWritePhase(const Action& action)
  {
    // The future 'writing' can only be discarded in 'finalize'.
    CHECK(!writing.isDiscarded());

    if (writing.isFailed()) {
      promise.fail("Write phase failed: " + writing.failure());
      terminate(self());
    } else {
      const WriteResponse& response = writing.get();
      if (!response.okay()) {
        // Retry with a higher proposal number.
        retry(response.proposal());
      } else {
        // The write has been accepted (and thus performed) by a
        // quorum of replicas. A consensus has been reached.
        Action learnedAction = action;
        learnedAction.set_learned(true);

        runLearnPhase(learnedAction);
      }
    }
  }

  void runLearnPhase(const Action& action)
  {
    CHECK(action.has_learned() && action.learned());

    // We need to make sure that the learned message has been
    // broadcasted before the fill process completes. Some users may
    // rely on this invariant (e.g. checking if the local replica has
    // learned the action).
    log::learn(network, action)
      .onAny(defer(self(), &Self::checkLearnPhase, action, lambda::_1));
  }

  void checkLearnPhase(const Action& action, const Future<Nothing>& future)
  {
    if (!future.isReady()) {
      promise.fail(
          future.isFailed() ?
          "Write phase failed: " + future.failure() :
          "Not expecting discarded future");
      terminate(self());
    } else {
      promise.set(action);
      terminate(self());
    }
  }

  void retry(uint64_t highestNackProposal)
  {
    // See comments below.
    static const Duration T = Milliseconds(100);

    // Bump the proposal number.
    CHECK_GE(highestNackProposal, proposal);
    proposal = highestNackProposal + 1;

    // Randomized back-off. Generate a random delay in [T, 2T). T has
    // to be chosen carefully. We want T >> broadcast time such that
    // one proposer usually times out and wins before others wake up.
    // On the other hand, we want T to be as small as possible such
    // that we can reduce the wait time.
    Duration d = T * (1.0 + (double) os::random() / RAND_MAX);
    delay(d, self(), &Self::runPromisePhase);
  }

  const size_t quorum;
  const Shared<Network> network;
  const uint64_t position;

  uint64_t proposal;

  process::Promise<Action> promise;
  Future<PromiseResponse> promising;
  Future<WriteResponse> writing;
};


/////////////////////////////////////////////////
// Public interfaces below.
/////////////////////////////////////////////////


Future<PromiseResponse> promise(
    size_t quorum,
    const Shared<Network>& network,
    uint64_t proposal,
    const Option<uint64_t>& position)
{
  if (position.isNone()) {
    ImplicitPromiseProcess* process =
      new ImplicitPromiseProcess(
          quorum,
          network,
          proposal);

    Future<PromiseResponse> future = process->future();
    spawn(process, true);
    return future;
  } else {
    ExplicitPromiseProcess* process =
      new ExplicitPromiseProcess(
          quorum,
          network,
          proposal,
          position.get());

    Future<PromiseResponse> future = process->future();
    spawn(process, true);
    return future;
  }
}


Future<WriteResponse> write(
    size_t quorum,
    const Shared<Network>& network,
    uint64_t proposal,
    const Action& action)
{
  WriteProcess* process =
    new WriteProcess(
        quorum,
        network,
        proposal,
        action);

  Future<WriteResponse> future = process->future();
  spawn(process, true);
  return future;
}


Future<Nothing> learn(const Shared<Network>& network, const Action& action)
{
  LearnedMessage message;
  message.mutable_action()->CopyFrom(action);

  if (!action.has_learned() || !action.learned()) {
    message.mutable_action()->set_learned(true);
  }

  return network->broadcast(message);
}


Future<Action> fill(
    size_t quorum,
    const Shared<Network>& network,
    uint64_t proposal,
    uint64_t position)
{
  FillProcess* process =
    new FillProcess(
        quorum,
        network,
        proposal,
        position);

  Future<Action> future = process->future();
  spawn(process, true);
  return future;
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
