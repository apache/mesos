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

#include <algorithm>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/none.hpp>

#include "common/type_utils.hpp"

#include "log/catchup.hpp"
#include "log/consensus.hpp"
#include "log/coordinator.hpp"

#include "messages/log.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace log {

class CoordinatorProcess : public Process<CoordinatorProcess>
{
public:
  CoordinatorProcess(
      size_t _quorum,
      const Shared<Replica>& _replica,
      const Shared<Network>& _network)
    : ProcessBase(ID::generate("log-coordinator")),
      quorum(_quorum),
      replica(_replica),
      network(_network),
      state(INITIAL),
      proposal(0),
      index(0) {}

  virtual ~CoordinatorProcess() {}

  // See comments in 'coordinator.hpp'.
  Future<Option<uint64_t> > elect();
  Future<uint64_t> demote();
  Future<uint64_t> append(const string& bytes);
  Future<uint64_t> truncate(uint64_t to);

protected:
  virtual void finalize()
  {
    electing.discard();
    writing.discard();
  }

private:
  /////////////////////////////////
  // Election related functions. //
  /////////////////////////////////

  Future<uint64_t> getLastProposal();
  Future<Nothing> updateProposal(uint64_t promised);
  Future<PromiseResponse> runPromisePhase();
  Future<Option<uint64_t> > checkPromisePhase(const PromiseResponse& response);
  Future<vector<uint64_t> > getMissingPositions();
  Future<Nothing> catchupMissingPositions(const vector<uint64_t>& positions);
  Future<Option<uint64_t> > updateIndexAfterElected();
  void electingFinished(const Option<uint64_t>& position);
  void electingFailed();
  void electingAborted();

  /////////////////////////////////
  // Writing related functions.  //
  /////////////////////////////////

  Future<uint64_t> write(const Action& action);
  Future<WriteResponse> runWritePhase(const Action& action);
  Future<Nothing> checkWritePhase(const WriteResponse& response);
  Future<Nothing> runLearnPhase(const Action& action);
  Future<bool> checkLearnPhase(const Action& action);
  Future<uint64_t> updateIndexAfterWritten(bool missing);
  void writingFinished();
  void writingFailed();
  void writingAborted();

  const size_t quorum;
  const Shared<Replica> replica;
  const Shared<Network> network;

  // The current state of the coordinator. A coordinator needs to be
  // elected first to perform append and truncate operations. If one
  // tries to do an append or a truncate while the coordinator is not
  // elected, a failed future will be returned immediately. A
  // coordinator does not declare itself as elected until it wins the
  // election and has filled all existing positions. A coordinator is
  // put in electing state after it decides to go for an election and
  // before it is elected.
  enum {
    INITIAL,
    ELECTING,
    ELECTED,
    WRITING,
  } state;

  // The current proposal number used by this coordinator.
  uint64_t proposal;

  // The position to which the next entry will be written.
  uint64_t index;

  Future<Option<uint64_t> > electing;
  Future<uint64_t> writing;
};


/////////////////////////////////////////////////
// Handles elect/demote in CoordinatorProcess.
/////////////////////////////////////////////////


Future<Option<uint64_t> > CoordinatorProcess::elect()
{
  if (state == ELECTING) {
    return Future<Option<uint64_t> >::failed(
        "Coordinator already being elected");
  } else if (state == ELECTED) {
    return Future<Option<uint64_t> >::failed(
        "Coordinator already elected");
  } else if (state == WRITING) {
    return Future<Option<uint64_t> >::failed(
        "Coordinator already elected, and is currently writing");
  }

  CHECK_EQ(state, INITIAL);

  state = ELECTING;

  electing = getLastProposal()
    .then(defer(self(), &Self::updateProposal, lambda::_1))
    .then(defer(self(), &Self::runPromisePhase))
    .then(defer(self(), &Self::checkPromisePhase, lambda::_1))
    .onReady(defer(self(), &Self::electingFinished, lambda::_1))
    .onFailed(defer(self(), &Self::electingFailed))
    .onDiscarded(defer(self(), &Self::electingAborted));

  return electing;
}


Future<uint64_t> CoordinatorProcess::getLastProposal()
{
  return replica->promised();
}


Future<Nothing> CoordinatorProcess::updateProposal(uint64_t promised)
{
  // It is possible that we have already tried an election and lost.
  // We save the proposal number used in the last election in field
  // 'proposal', and will try at least the proposal number we had
  // before or greater in the next election.
  proposal = std::max(proposal, promised) + 1;
  return Nothing();
}


Future<PromiseResponse> CoordinatorProcess::runPromisePhase()
{
  return log::promise(quorum, network, proposal);
}


Future<Option<uint64_t> > CoordinatorProcess::checkPromisePhase(
    const PromiseResponse& response)
{
  if (!response.okay()) {
    // Lost an election, but can be retried. We save the proposal
    // number here so that most likely we will have a high enough
    // proposal number when we retry.
    CHECK_LE(proposal, response.proposal());
    proposal = response.proposal();

    return None();
  } else {
    CHECK(response.has_position());
    index = response.position();

    // Need to "catch-up" local replica (i.e., fill in any unlearned
    // and/or missing positions) so that we can do local reads.
    // Usually we could do this lazily, however, a local learned
    // position might have been truncated, so we actually need to
    // catch-up the local replica all the way to the end of the log
    // before we can perform any up-to-date local reads.
    return getMissingPositions()
      .then(defer(self(), &Self::catchupMissingPositions, lambda::_1))
      .then(defer(self(), &Self::updateIndexAfterElected));
   }
}


Future<vector<uint64_t> > CoordinatorProcess::getMissingPositions()
{
  return replica->missing(0, index);
}


Future<Nothing> CoordinatorProcess::catchupMissingPositions(
    const vector<uint64_t>& positions)
{
  LOG(INFO) << "Coordinator attemping to fill missing position";

  return log::catchup(quorum, replica, network, proposal, positions);
}


Future<Option<uint64_t> > CoordinatorProcess::updateIndexAfterElected()
{
  return Option<uint64_t>(index++);
}


void CoordinatorProcess::electingFinished(const Option<uint64_t>& position)
{
  CHECK_EQ(state, ELECTING);

  if (position.isNone()) {
    state = INITIAL;
  } else {
    state = ELECTED;
  }
}


void CoordinatorProcess::electingFailed()
{
  CHECK_EQ(state, ELECTING);
  state = INITIAL;
}


void CoordinatorProcess::electingAborted()
{
  CHECK_EQ(state, ELECTING);
  state = INITIAL;
}


Future<uint64_t> CoordinatorProcess::demote()
{
  if (state == INITIAL) {
    return Future<uint64_t>::failed("Coordinator is not elected");
  } else if (state == ELECTING) {
    return Future<uint64_t>::failed("Coordinator is being elected");
  } else if (state == WRITING) {
    return Future<uint64_t>::failed("Coordinator is currently writing");
  }

  CHECK_EQ(state, ELECTED);

  state = INITIAL;
  return index - 1;
}


/////////////////////////////////////////////////
// Handles write in CoordinatorProcess.
/////////////////////////////////////////////////


Future<uint64_t> CoordinatorProcess::append(const string& bytes)
{
  if (state == INITIAL) {
    return Future<uint64_t>::failed("Coordinator is not elected");
  } else if (state == ELECTING) {
    return Future<uint64_t>::failed("Coordinator is being elected");
  } else if (state == WRITING) {
    return Future<uint64_t>::failed("Coordinator is currently writing");
  }

  Action action;
  action.set_position(index);
  action.set_promised(proposal);
  action.set_performed(proposal);
  action.set_type(Action::APPEND);
  Action::Append* append = action.mutable_append();
  append->set_bytes(bytes);

  return write(action);
}


Future<uint64_t> CoordinatorProcess::truncate(uint64_t to)
{
  if (state == INITIAL) {
    return Future<uint64_t>::failed("Coordinator is not elected");
  } else if (state == ELECTING) {
    return Future<uint64_t>::failed("Coordinator is being elected");
  } else if (state == WRITING) {
    return Future<uint64_t>::failed("Coordinator is currently writing");
  }

  Action action;
  action.set_position(index);
  action.set_promised(proposal);
  action.set_performed(proposal);
  action.set_type(Action::TRUNCATE);
  Action::Truncate* truncate = action.mutable_truncate();
  truncate->set_to(to);

  return write(action);
}


Future<uint64_t> CoordinatorProcess::write(const Action& action)
{
  LOG(INFO) << "Coordinator attempting to write " << action.type()
            << " action at position " << action.position();

  CHECK_EQ(state, ELECTED);
  CHECK(action.has_performed() && action.has_type());

  state = WRITING;

  writing = runWritePhase(action)
    .then(defer(self(), &Self::checkWritePhase, lambda::_1))
    .then(defer(self(), &Self::runLearnPhase, action))
    .then(defer(self(), &Self::checkLearnPhase, action))
    .then(defer(self(), &Self::updateIndexAfterWritten, lambda::_1))
    .onReady(defer(self(), &Self::writingFinished))
    .onFailed(defer(self(), &Self::writingFailed))
    .onDiscarded(defer(self(), &Self::writingAborted));

  return writing;
}


Future<WriteResponse> CoordinatorProcess::runWritePhase(const Action& action)
{
  return log::write(quorum, network, proposal, action);
}


Future<Nothing> CoordinatorProcess::checkWritePhase(
    const WriteResponse& response)
{
   if (!response.okay()) {
    // Received a NACK. Save the proposal number.
    CHECK_LE(proposal, response.proposal());
    proposal = response.proposal();

    return Future<Nothing>::failed("Coordinator demoted");
  } else {
    return Nothing();
  }
}


Future<Nothing> CoordinatorProcess::runLearnPhase(const Action& action)
{
  return log::learn(network, action);
}


Future<bool> CoordinatorProcess::checkLearnPhase(const Action& action)
{
  // Make sure that the local replica has learned the newly written
  // log entry. Since messages are delivered and dispatched in order
  // locally, we should always have the new entry learned by now.
  return replica->missing(action.position());
}


Future<uint64_t> CoordinatorProcess::updateIndexAfterWritten(bool missing)
{
  CHECK(!missing) << "Not expecting local replica to be missing position "
                  << index << " after the writing is done";

  return index++;
}


void CoordinatorProcess::writingFinished()
{
  CHECK_EQ(state, WRITING);
  state = ELECTED;
}


void CoordinatorProcess::writingFailed()
{
  CHECK_EQ(state, WRITING);
  state = INITIAL;
}


void CoordinatorProcess::writingAborted()
{
  CHECK_EQ(state, WRITING);
  state = ELECTED;
}


/////////////////////////////////////////////////
// Coordinator implementation.
/////////////////////////////////////////////////


Coordinator::Coordinator(
    size_t quorum,
    const Shared<Replica>& replica,
    const Shared<Network>& network)
{
  process = new CoordinatorProcess(quorum, replica, network);
  spawn(process);
}


Coordinator::~Coordinator()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Option<uint64_t> > Coordinator::elect()
{
  return dispatch(process, &CoordinatorProcess::elect);
}


Future<uint64_t> Coordinator::demote()
{
  return dispatch(process, &CoordinatorProcess::demote);
}


Future<uint64_t> Coordinator::append(const string& bytes)
{
  return dispatch(process, &CoordinatorProcess::append, bytes);
}


Future<uint64_t> Coordinator::truncate(uint64_t to)
{
  return dispatch(process, &CoordinatorProcess::truncate, to);
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
