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

#include <algorithm>

#include <mesos/type_utils.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>

#ifndef __WINDOWS__
#include "log/leveldb.hpp"
#endif // __WINDOWS__
#include "log/replica.hpp"
#include "log/storage.hpp"

using namespace process;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace log {

namespace protocol {

// Some replica protocol definitions.
Protocol<PromiseRequest, PromiseResponse> promise;
Protocol<WriteRequest, WriteResponse> write;
Protocol<RecoverRequest, RecoverResponse> recover;

} // namespace protocol {


class ReplicaProcess : public ProtobufProcess<ReplicaProcess>
{
public:
  // Constructs a new replica process using specified path to a
  // directory for storing the underlying log.
  explicit ReplicaProcess(const string& path);

  ~ReplicaProcess() override;

  // Returns the action associated with this position. A none result
  // means that no action is known for this position. An error result
  // means that there was an error while trying to get this action
  // (for example, going to disk to read the log may have
  // failed). Note that reading a position that has been learned to
  // be truncated will also return an error.
  Result<Action> read(uint64_t position);

  // Returns all the actions between the specified positions, unless
  // those positions are invalid, in which case returns an error.
  Future<list<Action>> read(uint64_t from, uint64_t to);

  // Returns true if the specified position is missing in the log
  // (i.e., unlearned or holes).
  bool missing(uint64_t position);

  // Returns missing positions in the log (i.e., unlearned or holes)
  // within the specified range [from, to].
  IntervalSet<uint64_t> missing(uint64_t from, uint64_t to);

  // Returns the beginning position of the log.
  uint64_t beginning();

  // Returns the last written position in the log.
  uint64_t ending();

  // Returns the current status of this replica.
  Metadata::Status status();

  // Returns the highest implicit promise this replica has given.
  uint64_t promised();

  // Updates the status of this replica. The update will be persisted
  // to storage. Returns true on success and false otherwise.
  bool update(const Metadata::Status& status);

private:
  // Handles a request from a proposer to promise not to accept writes
  // from any other proposer with lower proposal number.
  void promise(const UPID& from, const PromiseRequest& request);

  // Handles a request from a proposer to write an action.
  void write(const UPID& from, const WriteRequest& request);

  // Handles a request from a recover process.
  void recover(const UPID& from, const RecoverRequest& request);

  // Handles a message notifying of a learned action.
  void learned(const UPID& from, const Action& action);

  // Persists the specified action to storage. Returns true on success
  // and false otherwise.
  bool persist(const Action& action);

  // Updates the highest promise this replica has given. The update
  // will be persisted to storage. Returns true on success and false
  // otherwise.
  bool updatePromised(uint64_t promised);

  // Helper routine to restore log (e.g., on restart).
  void restore(const string& path);

  // Underlying storage for the log.
  Storage* storage;

  // The cached metadata for this replica. It includes the current
  // status of the replica and the last promise it made.
  Metadata metadata;

  // Beginning position of log (after *learned* truncations).
  uint64_t begin;

  // Ending position of log (last written position).
  uint64_t end;

  // Holes in the log.
  IntervalSet<uint64_t> holes;

  // Unlearned positions in the log.
  IntervalSet<uint64_t> unlearned;
};


ReplicaProcess::ReplicaProcess(const string& path)
  : ProcessBase(ID::generate("log-replica")),
    begin(0),
    end(0)
{
  // TODO(benh): Factor out and expose storage.
  storage = new LevelDBStorage();

  restore(path);

  // Install protobuf handlers.
  install<PromiseRequest>(
      &ReplicaProcess::promise);

  install<WriteRequest>(
      &ReplicaProcess::write);

  install<RecoverRequest>(
      &ReplicaProcess::recover);

  install<LearnedMessage>(
      &ReplicaProcess::learned,
      &LearnedMessage::action);
}


ReplicaProcess::~ReplicaProcess()
{
  delete storage;
}


Result<Action> ReplicaProcess::read(uint64_t position)
{
  if (position < begin) {
    return Error("Attempted to read truncated position");
  } else if (end < position) {
    return None(); // These semantics are assumed above!
  } else if (holes.contains(position)) {
    return None();
  }

  // Must exist in storage ...
  Try<Action> action = storage->read(position);

  if (action.isError()) {
    return Error(action.error());
  }

  return action.get();
}


// TODO(benh): Make this function actually return a Try once we change
// the future semantics to not include failures.
Future<list<Action>> ReplicaProcess::read(uint64_t from, uint64_t to)
{
  if (to < from) {
    process::Promise<list<Action>> promise;
    promise.fail("Bad read range (to < from)");
    return promise.future();
  } else if (from < begin) {
    process::Promise<list<Action>> promise;
    promise.fail("Bad read range (truncated position)");
    return promise.future();
  } else if (end < to) {
    process::Promise<list<Action>> promise;
    promise.fail("Bad read range (past end of log)");
    return promise.future();
  }

  VLOG(2) << "Starting read from '" << stringify(from) << "' to '"
          << stringify(to) << "'";

  list<Action> actions;

  for (uint64_t position = from; position <= to; position++) {
    Result<Action> result = read(position);

    if (result.isError()) {
      process::Promise<list<Action>> promise;
      promise.fail(result.error());
      return promise.future();
    } else if (result.isSome()) {
      actions.push_back(result.get());
    }
  }

  return actions;
}


bool ReplicaProcess::missing(uint64_t position)
{
  if (position < begin) {
    return false; // Truncated positions are treated as learned.
  } else if (position > end) {
    return true;
  } else {
    return unlearned.contains(position) || holes.contains(position);
  }
}


// TODO(jieyu): Allow this method to take an Interval.
IntervalSet<uint64_t> ReplicaProcess::missing(uint64_t from, uint64_t to)
{
  if (from > to) {
    // Empty interval.
    return IntervalSet<uint64_t>();
  }

  IntervalSet<uint64_t> positions;

  // Add unlearned positions.
  positions += unlearned;

  // Add holes.
  positions += holes;

  // Add all the unknown positions beyond our end.
  if (to > end) {
    positions += (Bound<uint64_t>::open(end), Bound<uint64_t>::closed(to));
  }

  // Do not consider positions outside [from, to].
  positions &= (Bound<uint64_t>::closed(from), Bound<uint64_t>::closed(to));

  return positions;
}


uint64_t ReplicaProcess::beginning()
{
  return begin;
}


uint64_t ReplicaProcess::ending()
{
  return end;
}


Metadata::Status ReplicaProcess::status()
{
  return metadata.status();
}


uint64_t ReplicaProcess::promised()
{
  return metadata.promised();
}


bool ReplicaProcess::update(const Metadata::Status& status)
{
  Metadata metadata_;
  metadata_.set_status(status);
  metadata_.set_promised(promised());

  Try<Nothing> persisted = storage->persist(metadata_);

  if (persisted.isError()) {
    LOG(ERROR) << "Error writing to log: " << persisted.error();
    return false;
  }

  LOG(INFO) << "Persisted replica status to " << status;

  // Update the cached metadata.
  metadata.set_status(status);

  return true;
}


bool ReplicaProcess::updatePromised(uint64_t promised)
{
  Metadata metadata_;
  metadata_.set_status(status());
  metadata_.set_promised(promised);

  Try<Nothing> persisted = storage->persist(metadata_);

  if (persisted.isError()) {
    LOG(ERROR) << "Error writing to log: " << persisted.error();
    return false;
  }

  LOG(INFO) << "Persisted promised to " << promised;

  // Update the cached metadata.
  metadata.set_promised(promised);

  return true;
}


// When handling replicated log protocol requests, we handle errors in
// three different ways:
//
//  1. If we've accepted a conflicting request with a higher proposal
//     number, we return a REJECT response.
//  2. If we can't vote on the request because we're in the wrong
//     state (e.g., not finished the recovery or catchup protocols),
//     we return an IGNORED response.
//  3. If we encounter an error (e.g., I/O failure) handling the
//     request, we log the error and silently ignore the request.
//
// TODO(benh): At some point, however, we might want to actually
// "fail" more dramatically for case three, because there could be
// something rather seriously wrong on this box that we are ignoring
// (like a bad disk).  This could be accomplished by changing most
// LOG(ERROR) statements to LOG(FATAL), or by counting the number of
// errors and after reaching some threshold aborting. In addition,
// sending the error information back to the proposer "might" help the
// debugging procedure.


void ReplicaProcess::promise(const UPID& from, const PromiseRequest& request)
{
  // Ignore promise requests if this replica is not in VOTING status;
  // we also inform the requester, so that they can retry promptly.
  if (status() != Metadata::VOTING) {
    LOG(INFO) << "Replica ignoring promise request from " << from
              << " as it is in " << status() << " status";

    PromiseResponse response;
    response.set_type(PromiseResponse::IGNORED);
    response.set_okay(false);
    response.set_proposal(request.proposal());
    reply(response);
    return;
  }

  if (request.has_position()) {
    LOG(INFO) << "Replica received explicit promise request from " << from
              << " for position " << request.position()
              << " with proposal " << request.proposal();

    // If the position has been truncated, tell the proposer that it's
    // a learned no-op. This can happen when a replica has missed some
    // truncates and its proposer tries to fill some truncated
    // positions on election. A learned no-op is safe since the
    // proposer should eventually learn that this position was
    // actually truncated. The action must be _learned_ so that the
    // proposer doesn't attempt to run a full Paxos round which will
    // never succeed because this replica will not permit the write
    // (because ReplicaProcess::write "ignores" writes on truncated
    // positions).
    // TODO(jieyu): Think about whether we need to check proposal
    // number so that we don't reply a proposer whose number is
    // obviously smaller than most of the proposers in the system.
    if (request.position() < begin) {
      Action action;
      action.set_position(request.position());
      action.set_promised(promised()); // Use the last promised proposal.
      action.set_performed(promised()); // Use the last promised proposal.
      action.set_learned(true);
      action.set_type(Action::NOP);
      action.mutable_nop()->MergeFrom(Action::Nop());
      action.mutable_nop()->set_tombstone(true);

      PromiseResponse response;
      response.set_type(PromiseResponse::ACCEPT);
      response.set_okay(true);
      response.set_proposal(request.proposal());
      response.mutable_action()->MergeFrom(action);
      reply(response);
      return;
    }

    // Need to get the action for the specified position.
    Result<Action> result = read(request.position());

    if (result.isError()) {
      LOG(ERROR) << "Error getting log record at " << request.position()
                 << ": " << result.error();
    } else if (result.isNone()) {
      // This position has been implicitly promised to a proposer.
      // Therefore, we should no longer give promise to a proposer
      // with a lower (or equal) proposal number. If not, we may
      // accept writes from both proposers, causing a potential
      // inconsistency in the log. For example, there are three
      // replicas R1, R2 and R3. Assume that log position 1 in all
      // replicas are implicitly promised to proposer 2. Later,
      // proposer 1 asks for explicit promises from R2 and R3 for log
      // position 1. If we don't perform the following check, R2 and
      // R3 will give their promises to R2 and R3 for log position 1.
      // As a result, proposer 1 can successfully write a value X to
      // log position 1 and thinks that X is agreed, while proposer 2
      // can later write a value Y and also believes that Y is agreed.
      if (request.proposal() <= promised()) {
        // If a promise request is rejected because of the proposal
        // number check, we reply with the currently promised proposal
        // number so that the proposer can bump its proposal number
        // and retry if needed to ensure liveness.
        PromiseResponse response;
        response.set_type(PromiseResponse::REJECT);
        response.set_okay(false);
        response.set_proposal(promised());
        reply(response);
      } else {
        Action action;
        action.set_position(request.position());
        action.set_promised(request.proposal());

        if (persist(action)) {
          PromiseResponse response;
          response.set_type(PromiseResponse::ACCEPT);
          response.set_okay(true);
          response.set_proposal(request.proposal());
          response.set_position(request.position());
          reply(response);
        }
      }
    } else {
      CHECK_SOME(result);
      Action action = result.get();
      CHECK_EQ(action.position(), request.position());

      if (request.proposal() <= action.promised()) {
        PromiseResponse response;
        response.set_type(PromiseResponse::REJECT);
        response.set_okay(false);
        response.set_proposal(action.promised());
        reply(response);
      } else {
        Action original = action;
        action.set_promised(request.proposal());

        if (persist(action)) {
          PromiseResponse response;
          response.set_type(PromiseResponse::ACCEPT);
          response.set_okay(true);
          response.set_proposal(request.proposal());
          response.mutable_action()->MergeFrom(original);
          reply(response);
        }
      }
    }
  } else {
    LOG(INFO) << "Replica received implicit promise request from " << from
              << " with proposal " << request.proposal();

    if (request.proposal() <= promised()) {
      // Only make an implicit promise once!
      LOG(INFO) << "Replica denying promise request with proposal "
                << request.proposal();
      PromiseResponse response;
      response.set_type(PromiseResponse::REJECT);
      response.set_okay(false);
      response.set_proposal(promised());
      reply(response);
    } else {
      if (updatePromised(request.proposal())) {
        // Return the last position written.
        PromiseResponse response;
        response.set_type(PromiseResponse::ACCEPT);
        response.set_okay(true);
        response.set_proposal(request.proposal());
        response.set_position(end);
        reply(response);
      }
    }
  }
}


void ReplicaProcess::write(const UPID& from, const WriteRequest& request)
{
  // Ignore write requests if this replica is not in VOTING status; we
  // also inform the requester, so that they can retry promptly.
  if (status() != Metadata::VOTING) {
    LOG(INFO) << "Replica ignoring write request from " << from
              << " as it is in " << status() << " status";

    WriteResponse response;
    response.set_type(WriteResponse::IGNORED);
    response.set_okay(false);
    response.set_proposal(request.proposal());
    response.set_position(request.position());
    reply(response);
    return;
  }

  LOG(INFO) << "Replica received write request for position "
            << request.position() << " from " << from;

  Result<Action> result = read(request.position());

  if (result.isError()) {
    LOG(ERROR) << "Error getting log record at " << request.position()
               << ": " << result.error();
  } else if (result.isNone()) {
    if (request.proposal() < promised()) {
      WriteResponse response;
      response.set_type(WriteResponse::REJECT);
      response.set_okay(false);
      response.set_proposal(promised());
      response.set_position(request.position());
      reply(response);
    } else {
      Action action;
      action.set_position(request.position());
      action.set_promised(promised());
      action.set_performed(request.proposal());
      if (request.has_learned()) action.set_learned(request.learned());
      action.set_type(request.type());

      switch (request.type()) {
        case Action::NOP:
          CHECK(request.has_nop());
          action.mutable_nop();
          break;
        case Action::APPEND:
          CHECK(request.has_append());
          action.mutable_append()->CopyFrom(request.append());
          break;
        case Action::TRUNCATE:
          CHECK(request.has_truncate());
          action.mutable_truncate()->CopyFrom(request.truncate());
          break;
        default:
          LOG(FATAL) << "Unknown Action::Type!";
      }

      if (persist(action)) {
        WriteResponse response;
        response.set_type(WriteResponse::ACCEPT);
        response.set_okay(true);
        response.set_proposal(request.proposal());
        response.set_position(request.position());
        reply(response);
      }
    }
  } else if (result.isSome()) {
    Action action = result.get();
    CHECK_EQ(action.position(), request.position());

    if (request.proposal() < action.promised()) {
      WriteResponse response;
      response.set_type(WriteResponse::REJECT);
      response.set_okay(false);
      response.set_proposal(action.promised());
      response.set_position(request.position());
      reply(response);
    } else {
      if (action.has_learned() && action.learned()) {
        // We ignore the write request if this position has already
        // been learned. Turns out that it is possible a replica
        // receives the learned message of a write earlier than the
        // write request of that write (Yes! It is possible! See
        // MESOS-1271 for details). In that case, we want to prevent
        // this log entry from being overwritten.
        //
        // TODO(neilc): Create a test-case for this scenario.
        //
        // TODO(benh): If the value in the write request is the same
        // as the learned value, consider sending back an ACK which
        // might speed convergence.
        //
        // NOTE: In the presence of truncations, we may encounter a
        // situation where this position has already been learned, but
        // we are receiving a write request for this position with a
        // different value. For example, assume that there are 5
        // replicas (R1 ~ R5). First, an append operation has been
        // agreed at position 5 by R1, R2, R3 and R4, but only R1
        // receives a learned message. Later, a truncate operation has
        // been agreed at position 10 by R1, R2 and R3, but only R1
        // receives a learned message. Now, a leader failover happens
        // and R5 is filled with a NOP at position 5 because its
        // coordinator receives a learned NOP at position 5 from R1
        // (because of its learned truncation at position 10). Now,
        // another leader failover happens and R4's coordinator tries
        // to fill position 5. However, it is only able to contact R2,
        // R3 and R4 during the explicit promise phase. Therefore, it
        // will try to write an append operation at position 5 to R5
        // while R5 currently have a learned NOP stored at position 5.
      } else {
        action.set_performed(request.proposal());
        action.clear_learned();
        if (request.has_learned()) action.set_learned(request.learned());
        action.clear_type();
        action.clear_nop();
        action.clear_append();
        action.clear_truncate();
        action.set_type(request.type());

        switch (request.type()) {
          case Action::NOP:
            CHECK(request.has_nop());
            action.mutable_nop();
            break;
          case Action::APPEND:
            CHECK(request.has_append());
            action.mutable_append()->CopyFrom(request.append());
            break;
          case Action::TRUNCATE:
            CHECK(request.has_truncate());
            action.mutable_truncate()->CopyFrom(request.truncate());
            break;
          default:
            LOG(FATAL) << "Unknown Action::Type!";
        }

        if (persist(action)) {
          WriteResponse response;
          response.set_type(WriteResponse::ACCEPT);
          response.set_okay(true);
          response.set_proposal(request.proposal());
          response.set_position(request.position());
          reply(response);
        }
      }
    }
  }
}


void ReplicaProcess::recover(const UPID& from, const RecoverRequest& request)
{
  LOG(INFO) << "Replica in " << status()
            << " status received a broadcasted recover request from "
            << from;

  RecoverResponse response;
  response.set_status(status());

  if (status() == Metadata::VOTING) {
    response.set_begin(begin);
    response.set_end(end);
  }

  reply(response);
}


void ReplicaProcess::learned(const UPID& from, const Action& action)
{
  LOG(INFO) << "Replica received learned notice for position "
            << action.position() << " from " << from;

  CHECK(action.learned());
  persist(action);
}


bool ReplicaProcess::persist(const Action& action)
{
  Try<Nothing> persisted = storage->persist(action);

  if (persisted.isError()) {
    LOG(ERROR) << "Error writing to log: " << persisted.error();
    return false;
  }

  VLOG(1) << "Persisted action " << action.type()
          << " at position " << action.position();

  // No longer a hole here (if there even was one).
  holes -= action.position();

  // Update unlearned positions and deal with truncation actions.
  if (action.has_learned() && action.learned()) {
    unlearned -= action.position();

    if (action.has_type() && action.type() == Action::TRUNCATE) {
      // No longer consider truncated positions as holes (so that a
      // coordinator doesn't try and fill them).
      holes -= (Bound<uint64_t>::open(0),
                Bound<uint64_t>::open(action.truncate().to()));

      // No longer consider truncated positions as unlearned (so that
      // a coordinator doesn't try and fill them).
      unlearned -= (Bound<uint64_t>::open(0),
                    Bound<uint64_t>::open(action.truncate().to()));

      // And update the beginning position.
      begin = std::max(begin, action.truncate().to());
    } else if (action.has_type() && action.type() == Action::NOP &&
               action.nop().has_tombstone() && action.nop().tombstone()) {
      // No longer consider truncated positions as holes (so that a
      // coordinator doesn't try and fill them).
      holes -= (Bound<uint64_t>::open(0),
                Bound<uint64_t>::open(action.position()));

      // No longer consider truncated positions as unlearned (so that
      // a coordinator doesn't try and fill them).
      unlearned -= (Bound<uint64_t>::open(0),
                    Bound<uint64_t>::open(action.position()));

      // And update the beginning position. There must exist at least
      // 1 position (TRUNCATE) in the log after the tombstone.
      begin = std::max(begin, action.position() + 1);
    }
  } else {
    // We just introduced an unlearned position.
    unlearned += action.position();
  }

  // Update holes if we just wrote many positions past the last end.
  if (action.position() > end) {
    holes += (Bound<uint64_t>::open(end),
              Bound<uint64_t>::open(action.position()));
  }

  // And update the end position.
  end = std::max(end, action.position());

  return true;
}


void ReplicaProcess::restore(const string& path)
{
  Try<Storage::State> state = storage->restore(path);

  if (state.isError()) {
    EXIT(EXIT_FAILURE) << "Failed to recover the log: " << state.error();
  }

  // Pull out and save some of the state.
  metadata = state->metadata;
  begin = state->begin;
  end = state->end;
  unlearned = state->unlearned;

  // Only use the learned positions to help determine the holes.
  const IntervalSet<uint64_t>& learned = state->learned;

  // Holes are those positions in [begin, end] that are not in both
  // learned and unlearned sets. In the case of a brand new log (begin
  // and end are 0, and learned and unlearned are empty), we assume
  // position 0 is a hole, and a coordinator will simply fill it with
  // a no-op when it first gets elected.
  holes += (Bound<uint64_t>::closed(begin), Bound<uint64_t>::closed(end));
  holes -= learned;
  holes -= unlearned;

  LOG(INFO) << "Replica recovered with log positions "
            << begin << " -> " << end
            << " with " << holes.size() << " holes"
            << " and " << unlearned.size() << " unlearned";
}


Replica::Replica(const string& path)
{
  process = new ReplicaProcess(path);
  spawn(process);
}


Replica::~Replica()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<list<Action>> Replica::read(uint64_t from, uint64_t to) const
{
  return dispatch(process, &ReplicaProcess::read, from, to);
}


Future<bool> Replica::missing(uint64_t position) const
{
  return dispatch(process, &ReplicaProcess::missing, position);
}


Future<IntervalSet<uint64_t>> Replica::missing(
    uint64_t from, uint64_t to) const
{
  return dispatch(process, &ReplicaProcess::missing, from, to);
}


Future<uint64_t> Replica::beginning() const
{
  return dispatch(process, &ReplicaProcess::beginning);
}


Future<uint64_t> Replica::ending() const
{
  return dispatch(process, &ReplicaProcess::ending);
}


Future<Metadata::Status> Replica::status() const
{
  return dispatch(process, &ReplicaProcess::status);
}


Future<uint64_t> Replica::promised() const
{
  return dispatch(process, &ReplicaProcess::promised);
}


Future<bool> Replica::update(const Metadata::Status& status)
{
  return dispatch(process, &ReplicaProcess::update, status);
}


PID<ReplicaProcess> Replica::pid() const
{
  return process->self();
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
