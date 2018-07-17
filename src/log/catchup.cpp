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

#include <list>

#include <process/collect.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/lambda.hpp>
#include <stout/stringify.hpp>

#include "log/catchup.hpp"
#include "log/consensus.hpp"
#include "log/recover.hpp"

#include "messages/log.hpp"

using namespace process;

using std::list;

namespace mesos {
namespace internal {
namespace log {

class CatchUpProcess : public Process<CatchUpProcess>
{
public:
  CatchUpProcess(
      size_t _quorum,
      const Shared<Replica>& _replica,
      const Shared<Network>& _network,
      uint64_t _proposal,
      uint64_t _position)
    : ProcessBase(ID::generate("log-catch-up")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      position(_position),
      proposal(_proposal) {}

  ~CatchUpProcess() override {}

  Future<uint64_t> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    check();
  }

  void finalize() override
  {
    checking.discard();
    filling.discard();

    // TODO(benh): Discard our promise only after 'checking' and
    // 'filling' have completed (ready, failed, or discarded).
    promise.discard();
  }

private:
  void check()
  {
    checking = replica->missing(position);
    checking.onAny(defer(self(), &Self::checked));
  }

  void checked()
  {
    // The future 'checking' can only be discarded in 'finalize'.
    CHECK(!checking.isDiscarded());

    if (checking.isFailed()) {
      promise.fail("Failed to get missing positions: " + checking.failure());
      terminate(self());
    } else if (!checking.get()) {
      // The position has been learned.
      promise.set(proposal);
      terminate(self());
    } else {
      // Still missing, try to fill it.
      fill();
    }
  }

  void fill()
  {
    filling = log::fill(quorum, network, proposal, position);
    filling.onAny(defer(self(), &Self::filled));
  }

  void filled()
  {
    // The future 'filling' can only be discarded in 'finalize'.
    CHECK(!filling.isDiscarded());

    if (filling.isFailed()) {
      promise.fail("Failed to fill missing position: " + filling.failure());
      terminate(self());
    } else {
      // Update the proposal number so that we can save a proposal
      // number bump round trip if we need to invoke fill again.
      CHECK(filling->promised() >= proposal);
      proposal = filling->promised();

      check();
    }
  }

  const size_t quorum;
  const Shared<Replica> replica;
  const Shared<Network> network;
  const uint64_t position;

  uint64_t proposal;

  process::Promise<uint64_t> promise;
  Future<bool> checking;
  Future<Action> filling;
};


// Catches-up a single log position in the local replica. This
// function returns the highest proposal number seen. The returned
// proposal number can be used to save extra proposal number bumps.
static Future<uint64_t> catchup(
    size_t quorum,
    const Shared<Replica>& replica,
    const Shared<Network>& network,
    uint64_t proposal,
    uint64_t position)
{
  CatchUpProcess* process =
    new CatchUpProcess(
        quorum,
        replica,
        network,
        proposal,
        position);

  Future<uint64_t> future = process->future();
  spawn(process, true);
  return future;
}


// TODO(jieyu): Our current implementation catches-up each position in
// the set sequentially. In the future, we may want to parallelize it
// to improve the performance. Also, we may want to implement rate
// control here so that we don't saturate the network or disk.
class BulkCatchUpProcess : public Process<BulkCatchUpProcess>
{
public:
  BulkCatchUpProcess(
      size_t _quorum,
      const Shared<Replica>& _replica,
      const Shared<Network>& _network,
      uint64_t _proposal,
      const Interval<uint64_t>& _positions,
      const Duration& _timeout)
    : ProcessBase(ID::generate("log-bulk-catch-up")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      positions(_positions),
      timeout(_timeout),
      proposal(_proposal) {}

  ~BulkCatchUpProcess() override {}

  Future<Nothing> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Catch-up sequentially.
    current = positions.lower();

    catchup();
  }

  void finalize() override
  {
    catching.discard();

    // TODO(benh): Discard our promise only after 'catching' has
    // completed (ready, failed, or discarded).
    promise.discard();
  }

private:
  static void timedout(Future<uint64_t> catching)
  {
    catching.discard();
  }

  void catchup()
  {
    if (current >= positions.upper()) {
      // Stop the process if there is nothing left to catch-up. This
      // also handles the case where the input interval is empty.
      promise.set(Nothing());
      terminate(self());
      return;
    }

    // Store the future so that we can discard it if the user wants to
    // cancel the catch-up operation.
    catching = log::catchup(quorum, replica, network, proposal, current)
      .onDiscarded(defer(self(), &Self::discarded))
      .onFailed(defer(self(), &Self::failed))
      .onReady(defer(self(), &Self::succeeded));

    Clock::timer(timeout, lambda::bind(&Self::timedout, catching));
  }


  void discarded()
  {
    LOG(INFO) << "Unable to catch-up position " << current
              << " in " << timeout << ", retrying";

    catchup();
  }

  void failed()
  {
    promise.fail(
        "Failed to catch-up position " + stringify(current) +
        ": " + catching.failure());

    terminate(self());
  }

  void succeeded()
  {
    ++current;

    // The single position catch-up function: 'log::catchup' will
    // return the highest proposal number seen so far. We use this
    // proposal number for the next 'catchup' as it is highly likely
    // that this number is high enough, saving potentially unnecessary
    // proposal number bumps.
    proposal = catching.get();

    catchup();
  }

  const size_t quorum;
  const Shared<Replica> replica;
  const Shared<Network> network;
  const Interval<uint64_t> positions;
  const Duration timeout;

  uint64_t proposal;
  uint64_t current;

  process::Promise<Nothing> promise;
  Future<uint64_t> catching;
};


static Future<Nothing> catchup(
    size_t quorum,
    const Shared<Replica>& replica,
    const Shared<Network>& network,
    const Option<uint64_t>& proposal,
    const Interval<uint64_t>& positions,
    const Duration& timeout)
{
  BulkCatchUpProcess* process =
    new BulkCatchUpProcess(
        quorum,
        replica,
        network,
        proposal.getOrElse(0),
        positions,
        timeout);

  Future<Nothing> future = process->future();
  spawn(process, true);
  return future;
}


// This process is used to catch-up missing positions in the local
// replica. We first check the status of the local replica. It if is
// not in VOTING status, the recover process will terminate
// immediately. Next we will run the log recover protocol to determine
// the log's beginning and ending positions. After that we will start
// doing catch-up.
class CatchupMissingProcess : public Process<CatchupMissingProcess>
{
public:
  CatchupMissingProcess(
      size_t _quorum,
      const Shared<Replica>& _replica,
      const Shared<Network>& _network,
      const Option<uint64_t>& _proposal,
      const Duration& _timeout)
    : ProcessBase(ID::generate("log-recover-missing")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      proposal(_proposal),
      timeout(_timeout) {}

  Future<uint64_t> future()
  {
    return promise.future();
  }

protected:
  void initialize() override
  {
    LOG(INFO) << "Starting missing positions recovery";

    // Register a callback to handle user initiated discard.
    promise.future().onDiscard(defer(self(), &Self::discard));

    // Check the current status of the local replica and decide if we
    // proceed with recovery. We do it only if the local replica is in
    // VOTING status.
    chain = replica->status()
      .then(defer(self(), &Self::recover, lambda::_1))
      .onAny(defer(self(), &Self::finished, lambda::_1));
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

  Future<Nothing> recover(const Metadata::Status& status)
  {
    LOG(INFO) << "Replica is in " << status << " status";

    if (status != Metadata::VOTING) {
      return Nothing();
    }

    return runRecoverProtocol(quorum, network, status, false)
      .then(defer(self(), &Self::_recover, lambda::_1));
  }

  Future<Nothing> _recover(const Option<RecoverResponse>& response)
  {
    if (response.isNone()) {
      return Failure("Failed to recover begin and end positions of the log");
    }

    if (response->status() != Metadata::RECOVERING) {
      return Failure("Unexpected status returned from the recover protocol");
    }

    CHECK(response->has_begin() && response->has_end());

    if (response->begin() == response->end()) {
      // This may happen if all replicas know only about position
      // 0 (just initialized).
      return Failure("Recovered only 1 position, cannot catch-up");
    }

    // We do not catchup the last recovered position in order to
    // prevent coordinator demotion.
    end = response->end() - 1;

    return replica->beginning()
      .then(defer(self(), [this, response](uint64_t begin) {
        // Ideally we would only need to catch-up positions from
        // the recovered range and then adjust local replica's
        // 'begin'. However, the replica needs to persist the
        // truncation indicator (TRUNCATE or a tombstone NOP) to
        // be able to recover the same 'begin' after a restart. If
        // we only catch-up positions from the recovered range, it
        // is possible that the replica sees neither TRUNCATE
        // (e.g. it is the last position in the log, which we
        // don't catch-up), nor a tombstone. We catch-up positions
        // starting with the lowest known begin position so that
        // the replica either retains the same 'begin', or sees a
        // truncation indicator.
        begin = std::min(begin, response->begin());
        return catchup(begin, end);
      }));
  }

  Future<Nothing> catchup(uint64_t begin, uint64_t end)
  {
    CHECK_LE(begin, end);

    LOG(INFO) << "Starting catch-up from position " << begin << " to " << end;

    IntervalSet<uint64_t> positions(
        Bound<uint64_t>::closed(begin),
        Bound<uint64_t>::closed(end));

    // TODO(ipronin): Consider using 'proposed' field from the local
    // replica.
    return log::catchup(quorum, replica, network, proposal, positions, timeout);
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
      promise.set(end);
      terminate(self());
    }
  }

  const size_t quorum;
  Shared<Replica> replica;
  const Shared<Network> network;
  const Option<uint64_t> proposal;
  const Duration timeout;

  Future<Nothing> chain;

  uint64_t end;

  process::Promise<uint64_t> promise;
};


/////////////////////////////////////////////////
// Public interfaces below.
/////////////////////////////////////////////////


Future<Nothing> catchup(
    size_t quorum,
    const Shared<Replica>& replica,
    const Shared<Network>& network,
    const Option<uint64_t>& proposal,
    const IntervalSet<uint64_t>& positions,
    const Duration& timeout)
{
  // Necessary to disambiguate overloaded functions.
  Future<Nothing> (*f)(
      size_t quorum,
      const Shared<Replica>& replica,
      const Shared<Network>& network,
      const Option<uint64_t>& proposal,
      const Interval<uint64_t>& positions,
      const Duration& timeout) = &catchup;

  Future<Nothing> future = Nothing();

  foreach (const Interval<uint64_t>& interval, positions) {
    future = future.then(
        lambda::bind(
            f,
            quorum,
            replica,
            network,
            proposal,
            interval,
            timeout));
  }

  return future;
}

Future<uint64_t> catchup(
    size_t quorum,
    const process::Shared<Replica>& replica,
    const process::Shared<Network>& network,
    const Option<uint64_t>& proposal,
    const Duration& timeout)
{
  CatchupMissingProcess* process =
    new CatchupMissingProcess(
        quorum,
        replica,
        network,
        proposal,
        timeout);

  Future<uint64_t> future = process->future();
  spawn(process, true);
  return future;
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
