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

#include <list>

#include <process/collect.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/lambda.hpp>
#include <stout/stringify.hpp>

#include "log/catchup.hpp"
#include "log/consensus.hpp"

#include "messages/log.hpp"

using namespace process;

using std::list;
using std::vector;

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

  virtual ~CatchUpProcess() {}

  Future<uint64_t> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    check();
  }

  virtual void finalize()
  {
    checking.discard();
    filling.discard();
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
      CHECK(filling.get().promised() >= proposal);
      proposal = filling.get().promised();

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
      const vector<uint64_t>& _positions,
      const Duration& _timeout)
    : ProcessBase(ID::generate("log-bulk-catch-up")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      positions(_positions),
      timeout(_timeout),
      proposal(_proposal) {}

  virtual ~BulkCatchUpProcess() {}

  Future<Nothing> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));

    // Catch-up sequentially.
    it = positions.begin();

    catchup();
  }

  virtual void finalize()
  {
    catching.discard();
  }

private:
  static void timedout(Future<uint64_t> catching)
  {
    catching.discard();
  }

  void catchup()
  {
    if (it == positions.end()) {
      promise.set(Nothing());
      terminate(self());
      return;
    }

    // Store the future so that we can discard it if the user wants to
    // cancel the catch-up operation.
    catching = log::catchup(quorum, replica, network, proposal, *it)
      .onDiscarded(defer(self(), &Self::discarded))
      .onFailed(defer(self(), &Self::failed))
      .onReady(defer(self(), &Self::succeeded));

    Timer::create(timeout, lambda::bind(&Self::timedout, catching));
  }

  void discarded()
  {
    LOG(INFO) << "Unable to catch-up position " << *it
              << " in " << timeout << ", retrying";

    catchup();
  }

  void failed()
  {
    promise.fail(
        "Failed to catch-up position " + stringify(*it) +
        ": " + catching.failure());

    terminate(self());
  }

  void succeeded()
  {
    ++it;

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
  const vector<uint64_t> positions;
  const Duration timeout;

  uint64_t proposal;
  vector<uint64_t>::const_iterator it;

  process::Promise<Nothing> promise;
  Future<uint64_t> catching;
};


/////////////////////////////////////////////////
// Public interfaces below.
/////////////////////////////////////////////////


Future<Nothing> catchup(
    size_t quorum,
    const Shared<Replica>& replica,
    const Shared<Network>& network,
    const Option<uint64_t>& proposal,
    const vector<uint64_t>& positions,
    const Duration& timeout)
{
  BulkCatchUpProcess* process =
    new BulkCatchUpProcess(
        quorum,
        replica,
        network,
        proposal.get(0),
        positions,
        timeout);

  Future<Nothing> future = process->future();
  spawn(process, true);
  return future;
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
