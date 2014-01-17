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

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <leveldb/comparator.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <algorithm>

#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/stopwatch.hpp>
#include <stout/utils.hpp>

#include "common/type_utils.hpp"

#include "log/replica.hpp"

#include "logging/logging.hpp"

#include "messages/log.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::list;
using std::set;
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


struct State
{
  Metadata metadata; // The metadata for the replica.
  uint64_t begin; // Beginning position of the log.
  uint64_t end; // Ending position of the log.
  set<uint64_t> learned; // Positions present and learned
  set<uint64_t> unlearned; // Positions present but unlearned.
};


// Abstract interface for reading and writing records.
class Storage
{
public:
  virtual ~Storage() {}
  virtual Try<State> restore(const string& path) = 0;
  virtual Try<Nothing> persist(const Metadata& metadata) = 0;
  virtual Try<Nothing> persist(const Action& action) = 0;
  virtual Try<Action> read(uint64_t position) = 0;
};


// Concrete implementation of the storage interface using leveldb.
class LevelDBStorage : public Storage
{
public:
  LevelDBStorage();
  virtual ~LevelDBStorage();

  virtual Try<State> restore(const string& path);
  virtual Try<Nothing> persist(const Metadata& metadata);
  virtual Try<Nothing> persist(const Action& action);
  virtual Try<Action> read(uint64_t position);

private:
  class Varint64Comparator : public leveldb::Comparator
  {
  public:
    virtual int Compare(
        const leveldb::Slice& a,
        const leveldb::Slice& b) const
    {
      // TODO(benh): Use varint comparator.
      LOG(FATAL) << "Unimplemented";
      // uint64_t left = position(a);
      // uint64_t right = position(b);
      // if (left < right) return -1;
      // if (left == right) return 0;
      // if (left > right) return 1;
    }

    virtual const char* Name() const
    {
      // Note that this name MUST NOT CHANGE across uses of this
      // comparator with the same DB (the semantics of doing so are
      // undefined if the database doesn't catch this first).
      return "varint64";
    }

    virtual void FindShortestSeparator(
        string* start,
        const leveldb::Slice& limit) const
    {
      // Intentional no-op.
    }

    virtual void FindShortSuccessor(string* key) const
    {
      // Intentional no-op.
    }
  };

  // Returns a string representing the specified position. Note that
  // we adjust the actual position by incrementing it by 1 because we
  // reserve 0 for storing the promise record (Record::Promise,
  // DEPRECATED!), or the metadata (Record::Metadata).
  static string encode(uint64_t position, bool adjust = true)
  {
    // Adjusted stringified represenation is plus 1 of actual position.
    position = adjust ? position + 1 : position;

    // TODO(benh): Use varint encoding for VarInt64Comparator!
    // string s;
    // google::protobuf::io::StringOutputStream _stream(&s);
    // google::protobuf::io::CodedOutputStream stream(&_stream);
    // position = adjust ? position + 1 : position;
    // stream.WriteVarint64(position);
    // return s;

    Try<string> s = strings::format("%.*d", 10, position);
    CHECK_SOME(s);
    return s.get();
  }

  // Returns the position as represented in the specified slice
  // (performing a decrement as necessary to determine the actual
  // position represented).
  static uint64_t decode(const leveldb::Slice& s)
  {
    // TODO(benh): Use varint decoding for VarInt64Comparator!
    // uint64_t position;
    // google::protobuf::io::ArrayInputStream _stream(s.data(), s.size());
    // google::protobuf::io::CodedInputStream stream(&_stream);
    // bool success = stream.ReadVarint64(&position);
    // CHECK(success);
    // return position - 1; // Actual position is less 1 of stringified.
    Try<uint64_t> position = numify<uint64_t>(string(s.data(), s.size()));
    CHECK_SOME(position);
    return position.get() - 1; // Actual position is less 1 of stringified.
  }

  // Varint64Comparator comparator; // TODO(benh): Use varint comparator.

  leveldb::DB* db;

  uint64_t first; // First position still in leveldb, used during truncation.
};


LevelDBStorage::LevelDBStorage()
  : db(NULL), first(0)
{
  // Nothing to see here.
}


LevelDBStorage::~LevelDBStorage()
{
  delete db; // Might be null if open failed in LevelDBStorage::recover.
}


Try<State> LevelDBStorage::restore(const string& path)
{
  leveldb::Options options;
  options.create_if_missing = true;

  // TODO(benh): Can't use varint comparator until bug discussed at
  // groups.google.com/group/leveldb/browse_thread/thread/17eac39168909ba7
  // gets fixed. For now, we are using the default byte-wise
  // comparator and *assuming* that the encoding from unsigned long to
  // string produces a stable ordering. Checks below.
  // options.comparator = &comparator;

  const string& one = encode(1);
  const string& two = encode(2);
  const string& ten = encode(10);

  CHECK(leveldb::BytewiseComparator()->Compare(one, two) < 0);
  CHECK(leveldb::BytewiseComparator()->Compare(two, one) > 0);
  CHECK(leveldb::BytewiseComparator()->Compare(one, ten) < 0);
  CHECK(leveldb::BytewiseComparator()->Compare(ten, two) > 0);
  CHECK(leveldb::BytewiseComparator()->Compare(ten, ten) == 0);

  Stopwatch stopwatch;
  stopwatch.start();

  leveldb::Status status = leveldb::DB::Open(options, path, &db);

  if (!status.ok()) {
    // TODO(benh): Consider trying to repair the DB.
    return Error(status.ToString());
  }

  LOG(INFO) << "Opened db in " << stopwatch.elapsed();

  stopwatch.start(); // Restart the stopwatch.

  // TODO(benh): Conditionally compact to avoid long recovery times?
  db->CompactRange(NULL, NULL);

  LOG(INFO) << "Compacted db in " << stopwatch.elapsed();

  State state;
  state.begin = 0;
  state.end = 0;

  // TODO(benh): Consider just reading the "promise" record (e.g.,
  // 'encode(0, false)') and then iterating over the rest of the
  // records and confirming that they are all indeed of type
  // Record::Action.

  stopwatch.start(); // Restart the stopwatch.

  leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());

  LOG(INFO) << "Created db iterator in " << stopwatch.elapsed();

  stopwatch.start(); // Restart the stopwatch.

  iterator->SeekToFirst();

  LOG(INFO) << "Seeked to beginning of db in " << stopwatch.elapsed();

  stopwatch.start(); // Restart the stopwatch.

  uint64_t keys = 0;

  while (iterator->Valid()) {
    keys++;
    const leveldb::Slice& slice = iterator->value();

    google::protobuf::io::ArrayInputStream stream(slice.data(), slice.size());

    Record record;

    if (!record.ParseFromZeroCopyStream(&stream)) {
      return Error("Failed to deserialize record");
    }

    switch (record.type()) {
      case Record::METADATA: {
        CHECK(record.has_metadata());
        state.metadata.CopyFrom(record.metadata());
        break;
      }

      // DEPRECATED!
      case Record::PROMISE: {
        CHECK(record.has_promise());
        // This replica is in old format. Set its status to VOTING
        // since there is no catch-up logic in the old code and this
        // replica is obviously not empty.
        state.metadata.set_status(Metadata::VOTING);
        state.metadata.set_promised(record.promise().proposal());
        break;
      }

      case Record::ACTION: {
        CHECK(record.has_action());
        const Action& action = record.action();
        if (action.has_learned() && action.learned()) {
          state.learned.insert(action.position());
          state.unlearned.erase(action.position());
          if (action.has_type() && action.type() == Action::TRUNCATE) {
            state.begin = std::max(state.begin, action.truncate().to());
          }
        } else {
          state.learned.erase(action.position());
          state.unlearned.insert(action.position());
        }
        state.end = std::max(state.end, action.position());
        break;
      }

      default: {
        return Error("Bad record");
      }
    }

    iterator->Next();
  }

  LOG(INFO) << "Iterated through " << keys
            << " keys in the db in " << stopwatch.elapsed();

  // Determine the first position still in leveldb so during a
  // truncation we can attempt to delete all positions from the first
  // position up to the truncate position. Note that this is not the
  // beginning position of the log, but rather the first position that
  // remains (i.e., hasn't been deleted) in leveldb.
  iterator->Seek(encode(0));

  if (iterator->Valid()) {
    first = decode(iterator->key());
  }

  delete iterator;

  return state;
}


Try<Nothing> LevelDBStorage::persist(const Metadata& metadata)
{
  Stopwatch stopwatch;
  stopwatch.start();

  leveldb::WriteOptions options;
  options.sync = true;

  Record record;
  record.set_type(Record::METADATA);
  record.mutable_metadata()->CopyFrom(metadata);

  string value;

  if (!record.SerializeToString(&value)) {
    return Error("Failed to serialize record");
  }

  leveldb::Status status = db->Put(options, encode(0, false), value);

  if (!status.ok()) {
    return Error(status.ToString());
  }

  LOG(INFO) << "Persisting metadata (" << value.size()
            << " bytes) to leveldb took " << stopwatch.elapsed();

  return Nothing();
}


Try<Nothing> LevelDBStorage::persist(const Action& action)
{
  Stopwatch stopwatch;
  stopwatch.start();

  Record record;
  record.set_type(Record::ACTION);
  record.mutable_action()->MergeFrom(action);

  string value;

  if (!record.SerializeToString(&value)) {
    return Error("Failed to serialize record");
  }

  leveldb::WriteOptions options;
  options.sync = true;

  leveldb::Status status = db->Put(options, encode(action.position()), value);

  if (!status.ok()) {
    return Error(status.ToString());
  }

  LOG(INFO) << "Persisting action (" << value.size()
            << " bytes) to leveldb took " << stopwatch.elapsed();

  // Delete positions if a truncate action has been *learned*. Note
  // that we do this in a best-effort fashion (i.e., we ignore any
  // failures to the database since we can always try again).
  if (action.has_type() && action.type() == Action::TRUNCATE &&
      action.has_learned() && action.learned()) {
    CHECK(action.has_truncate());

    stopwatch.start(); // Restart the stopwatch.

    // To actually perform the truncation in leveldb we need to remove
    // all the keys that represent positions no longer in the log. We
    // do this by attempting to delete all keys that represent the
    // first position we know is still in leveldb up to (but
    // excluding) the truncate position. Note that this works because
    // the semantics of WriteBatch are such that even if the position
    // doesn't exist (which is possible because this replica has some
    // holes), we can attempt to delete the key that represents it and
    // it will just ignore that key. This is *much* cheaper than
    // actually iterating through the entire database instead (which
    // was, for posterity, the original implementation). In addition,
    // caching the "first" position we know is in the database is
    // cheaper than using an iterator to determine the first position
    // (which was, for posterity, the second implementation).

    leveldb::WriteBatch batch;

    // Add positions up to (but excluding) the truncate position to
    // the batch starting at the first position still in leveldb.
    uint64_t index = 0;
    while ((first + index) < action.truncate().to()) {
      batch.Delete(encode(first + index));
      index++;
    }

    // If we added any positions, attempt to delete them!
    if (index > 0) {
      // We do this write asynchronously (e.g., using default options).
      leveldb::Status status = db->Write(leveldb::WriteOptions(), &batch);

      if (!status.ok()) {
        LOG(WARNING) << "Ignoring leveldb batch delete failure: "
                     << status.ToString();
      } else {
        first = action.truncate().to(); // Save the new first position!

        LOG(INFO) << "Deleting ~" << index
                  << " keys from leveldb took " << stopwatch.elapsed();
      }
    }
  }

  return Nothing();
}


Try<Action> LevelDBStorage::read(uint64_t position)
{
  Stopwatch stopwatch;
  stopwatch.start();

  leveldb::ReadOptions options;

  string value;

  leveldb::Status status = db->Get(options, encode(position), &value);

  if (!status.ok()) {
    return Error(status.ToString());
  }

  google::protobuf::io::ArrayInputStream stream(value.data(), value.size());

  Record record;

  if (!record.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize record");
  }

  if (record.type() != Record::ACTION) {
    return Error("Bad record");
  }

  LOG(INFO) << "Reading position from leveldb took " << stopwatch.elapsed();

  return record.action();
}


class ReplicaProcess : public ProtobufProcess<ReplicaProcess>
{
public:
  // Constructs a new replica process using specified path to a
  // directory for storing the underlying log.
  ReplicaProcess(const string& path);

  virtual ~ReplicaProcess();

  // Returns the action associated with this position. A none result
  // means that no action is known for this position. An error result
  // means that there was an error while trying to get this action
  // (for example, going to disk to read the log may have
  // failed). Note that reading a position that has been learned to
  // be truncated will also return an error.
  Result<Action> read(uint64_t position);

  // Returns all the actions between the specified positions, unless
  // those positions are invalid, in which case returns an error.
  Future<list<Action> > read(uint64_t from, uint64_t to);

  // Returns true if the specified position is missing in the log
  // (i.e., unlearned or holes).
  bool missing(uint64_t position);

  // Returns missing positions in the log (i.e., unlearned or holes)
  // within the specified range [from, to].
  set<uint64_t> missing(uint64_t from, uint64_t to);

  // Returns the beginning position of the log.
  uint64_t beginning();

  // Returns the last written position in the log.
  uint64_t ending();

  // Returns the current status of the this replica.
  Metadata::Status status();

  // Returns the highest implicit promise this replica has given.
  uint64_t promised();

  // Updates the status of this replica. The update will persisted on
  // the disk. Returns true on success and false otherwise.
  bool update(const Metadata::Status& status);

private:
  // Handles a request from a proposer to promise not to accept writes
  // from any other proposer with lower proposal number.
  void promise(const PromiseRequest& request);

  // Handles a request from a proposer to write an action.
  void write(const WriteRequest& request);

  // Handles a request from a recover process.
  void recover(const RecoverRequest& request);

  // Handles a message notifying of a learned action.
  void learned(const Action& action);

  // Helper routines that write a record corresponding to the
  // specified argument. Returns true on success and false otherwise.
  bool persist(const Action& action);

  // Helper routines that update metadata corresponding to the
  // specified argument. The update will be persisted on the disk.
  // Returns true on success and false otherwise.
  bool update(uint64_t promised);

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
  set<uint64_t> holes;

  // Unlearned positions in the log.
  set<uint64_t> unlearned;
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
  } else if (holes.count(position) > 0) {
    return None();
  }

  // Must exist in storage ...
  Try<Action> action = storage->read(position);

  if (action.isError()) {
    return Error(action.error());
  }

  CHECK_SOME(action);

  return action.get();
}


// TODO(benh): Make this function actually return a Try once we change
// the future semantics to not include failures.
Future<list<Action> > ReplicaProcess::read(uint64_t from, uint64_t to)
{
  if (to < from) {
    process::Promise<list<Action> > promise;
    promise.fail("Bad read range (to < from)");
    return promise.future();
  } else if (from < begin) {
    process::Promise<list<Action> > promise;
    promise.fail("Bad read range (truncated position)");
    return promise.future();
  } else if (end < to) {
    process::Promise<list<Action> > promise;
    promise.fail("Bad read range (past end of log)");
    return promise.future();
  }

  list<Action> actions;

  for (uint64_t position = from; position <= to; position++) {
    Result<Action> result = read(position);

    if (result.isError()) {
      process::Promise<list<Action> > promise;
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
    if (unlearned.count(position) != 0 || holes.count(position) != 0) {
      return true;
    } else {
      return false;
    }
  }
}


set<uint64_t> ReplicaProcess::missing(uint64_t from, uint64_t to)
{
  // TODO(jieyu): Optimize the performence for the common case.
  set<uint64_t> positions;

  // Add unlearned positions.
  foreach (uint64_t p, unlearned) {
    if (p >= from && p <= to) {
      positions.insert(p);
    }
  }

  // Add holes.
  foreach (uint64_t p, holes) {
    if (p >= from && p <= to) {
      positions.insert(p);
    }
  }

  // Add all the unknown positions beyond our end.
  for (; to > end; to--) {
    positions.insert(to);
  }

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


bool ReplicaProcess::update(uint64_t promised)
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


// Note that certain failures that occur result in returning from the
// current function but *NOT* sending a NACK back to the proposer
// because that implies a proposer has been demoted. Not sending
// anything is equivalent to pretending like the request never made it
// here. TODO(benh): At some point, however, we might want to actually
// "fail" more dramatically because there could be something rather
// seriously wrong on this box that we are ignoring (like a bad disk).
// This could be accomplished by changing most LOG(ERROR) statements
// to LOG(FATAL), or by counting the number of errors and after
// reaching some threshold aborting. In addition, sending the error
// information back to the proposer "might" help the debugging
// procedure.


void ReplicaProcess::promise(const PromiseRequest& request)
{
  // Ignore promise requests if this replica is not in VOTING status.
  if (status() != Metadata::VOTING) {
    LOG(INFO) << "Replica ignoring promise request as it is in "
              << status() << " status";
    return;
  }

  if (request.has_position()) {
    LOG(INFO) << "Replica received explicit promise request for position "
              << request.position() << " with proposal " << request.proposal();

    // If the position has been truncated, tell the proposer that it's
    // a learned no-op. This can happen when a replica has missed some
    // truncates and it's proposer tries to fill some truncated
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

      PromiseResponse response;
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
        response.set_okay(false);
        response.set_proposal(promised());
        reply(response);
      } else {
        Action action;
        action.set_position(request.position());
        action.set_promised(request.proposal());

        if (persist(action)) {
          PromiseResponse response;
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
        response.set_okay(false);
        response.set_proposal(action.promised());
        reply(response);
      } else {
        Action original = action;
        action.set_promised(request.proposal());

        if (persist(action)) {
          PromiseResponse response;
          response.set_okay(true);
          response.set_proposal(request.proposal());
          response.mutable_action()->MergeFrom(original);
          reply(response);
        }
      }
    }
  } else {
    LOG(INFO) << "Replica received implicit promise request with proposal "
              << request.proposal();

    if (request.proposal() <= promised()) {
      // Only make an implicit promise once!
      LOG(INFO) << "Replica denying promise request with proposal "
                << request.proposal();
      PromiseResponse response;
      response.set_okay(false);
      response.set_proposal(promised());
      reply(response);
    } else {
      if (update(request.proposal())) {
        // Return the last position written.
        PromiseResponse response;
        response.set_okay(true);
        response.set_proposal(request.proposal());
        response.set_position(end);
        reply(response);
      }
    }
  }
}


void ReplicaProcess::write(const WriteRequest& request)
{
  // Ignore write requests if this replica is not in VOTING status.
  if (status() != Metadata::VOTING) {
    LOG(INFO) << "Replica ignoring write request as it is in "
              << status() << " status";
    return;
  }

  LOG(INFO) << "Replica received write request for position "
            << request.position();

  Result<Action> result = read(request.position());

  if (result.isError()) {
    LOG(ERROR) << "Error getting log record at " << request.position()
               << ": " << result.error();
  } else if (result.isNone()) {
    if (request.proposal() < promised()) {
      WriteResponse response;
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
      response.set_okay(false);
      response.set_proposal(action.promised());
      response.set_position(request.position());
      reply(response);
    } else {
      // TODO(benh): Check if this position has already been learned,
      // and if so, check that we are re-writing the same value!
      //
      // TODO(jieyu): Interestingly, in the presence of truncations,
      // we may encounter a situation where this position has already
      // been learned, but we are re-writing a different value. For
      // example, assume that there are 5 replicas (R1 ~ R5). First,
      // an append operation has been agreed at position 5 by R1, R2,
      // R3 and R4, but only R1 receives a learned message. Later, a
      // truncate operation has been agreed at position 10 by R1, R2
      // and R3, but only R1 receives a learned message. Now, a leader
      // failover happens and R5 is filled with a NOP at position 5
      // because its coordinator receives a learned NOP at position 5
      // from R1 (because of its learned truncation at position 10).
      // Now, another leader failover happens and R4's coordinator
      // tries to fill position 5. However, it is only able to contact
      // R2, R3 and R4 during the explicit promise phase. As a result,
      // it will try to write an append operation at position 5 to R5
      // while R5 currently have a learned NOP stored at position 5.
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
        response.set_okay(true);
        response.set_proposal(request.proposal());
        response.set_position(request.position());
        reply(response);
      }
    }
  }
}


void ReplicaProcess::recover(const RecoverRequest& request)
{
  LOG(INFO) << "Replica in " << status()
            << " status received a broadcasted recover request";

  RecoverResponse response;
  response.set_status(status());

  if (status() == Metadata::VOTING) {
    response.set_begin(begin);
    response.set_end(end);
  }

  reply(response);
}


void ReplicaProcess::learned(const Action& action)
{
  LOG(INFO) << "Replica received learned notice for position "
            << action.position();

  CHECK(action.learned());

  if (persist(action)) {
    LOG(INFO) << "Replica learned " << action.type()
              << " action at position " << action.position();
  }
}


bool ReplicaProcess::persist(const Action& action)
{
  Try<Nothing> persisted = storage->persist(action);

  if (persisted.isError()) {
    LOG(ERROR) << "Error writing to log: " << persisted.error();
    return false;
  }

  LOG(INFO) << "Persisted action at " << action.position();

  // No longer a hole here (if there even was one).
  holes.erase(action.position());

  // Update unlearned positions and deal with truncation actions.
  if (action.has_learned() && action.learned()) {
    unlearned.erase(action.position());
    if (action.has_type() && action.type() == Action::TRUNCATE) {
      // No longer consider truncated positions as holes (so that a
      // coordinator doesn't try and fill them).
      foreach (uint64_t position, utils::copy(holes)) {
        if (position < action.truncate().to()) {
          holes.erase(position);
        }
      }

      // No longer consider truncated positions as unlearned (so that
      // a coordinator doesn't try and fill them).
      foreach (uint64_t position, utils::copy(unlearned)) {
        if (position < action.truncate().to()) {
          unlearned.erase(position);
        }
      }

      // And update the beginning position.
      begin = std::max(begin, action.truncate().to());
    }
  } else {
    // We just introduced an unlearned position.
    unlearned.insert(action.position());
  }

  // Update holes if we just wrote many positions past the last end.
  for (uint64_t position = end + 1; position < action.position(); position++) {
    holes.insert(position);
  }

  // And update the end position.
  end = std::max(end, action.position());

  return true;
}


void ReplicaProcess::restore(const string& path)
{
  Try<State> state = storage->restore(path);

  CHECK_SOME(state) << "Failed to recover the log";

  // Pull out and save some of the state.
  metadata = state.get().metadata;
  begin = state.get().begin;
  end = state.get().end;
  unlearned = state.get().unlearned;

  // Only use the learned positions to help determine the holes.
  const set<uint64_t>& learned = state.get().learned;

  // We need to assume that position 0 is a hole for a brand new log
  // (a coordinator will simply fill it with a no-op when it first
  // gets elected), unless the position was found during recovery or
  // it has been truncated.
  if (learned.count(0) == 0 && unlearned.count(0) == 0 && begin == 0) {
    holes.insert(0);
  }

  // Now determine the rest of the holes.
  for (uint64_t position = begin; position < end; position++) {
    if (learned.count(position) == 0 && unlearned.count(position) == 0) {
      holes.insert(position);
    }
  }

  LOG(INFO) << "Replica recovered with log positions "
            << begin << " -> " << end
            << " and holes " << stringify(holes)
            << " and unlearned " << stringify(unlearned);
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


Future<list<Action> > Replica::read(uint64_t from, uint64_t to) const
{
  return dispatch(process, &ReplicaProcess::read, from, to);
}


Future<bool> Replica::missing(uint64_t position) const
{
  return dispatch(process, &ReplicaProcess::missing, position);
}


Future<set<uint64_t> > Replica::missing(uint64_t from, uint64_t to) const
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
  // Need to disambiguate overloaded function.
  bool (ReplicaProcess::*update)(const Metadata::Status& status) =
    &ReplicaProcess::update;

  return dispatch(process, update, status);
}


PID<ReplicaProcess> Replica::pid() const
{
  return process->self();
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
