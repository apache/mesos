#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <list>
#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/mutex.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "log/log.hpp"

#include "state/log.hpp"

using namespace mesos::internal::log;
using namespace process;

// Note that we don't add 'using std::set' here because we need
// 'std::' to disambiguate the 'set' member.
using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace state {

// A storage implementation for State that uses the replicated
// log. The log is made up of appended operations. Each state entry is
// mapped to a log "snapshot".
//
// All operations are gated by 'start()' which makes sure that a
// Log::Writer has been started and all positions in the log have been
// read and cached in memory. All reads are performed by this cache
// (for now). If the Log::Writer gets demoted (i.e., because another
// writer started) then the current operation will return false
// implying the operation was not atomic and subsequent operations
// will re-'start()' which will again read all positions to make sure
// operations are consistent.
// TODO(benh): Log demotion does not necessarily imply a non-atomic
// read/modify/write. An alternative strategy might be to retry after
// restarting via 'start' (and holding on to the mutex so no other
// operations are attempted).
class LogStorageProcess : public Process<LogStorageProcess>
{
public:
  LogStorageProcess(Log* log);

  virtual ~LogStorageProcess();

  // Storage implementation.
  Future<Option<state::Entry> > get(const string& name);
  Future<bool> set(const state::Entry& entry, const UUID& uuid);
  Future<bool> expunge(const state::Entry& entry);
  Future<std::set<string> > names();

protected:
  virtual void finalize();

private:
  Future<Nothing> start();
  Future<Nothing> _start(const Option<Log::Position>& position);
  Future<Nothing> __start(
      const Log::Position& beginning,
      const Log::Position& position);

  // Helper for applying log entries.
  Future<Nothing> apply(const list<Log::Entry>& entries);

  // Helper for performing truncation.
  void truncate();
  Future<Nothing> _truncate();
  Future<Nothing> __truncate(
      const Log::Position& minimum,
      const Option<Log::Position>& position);

  // Continuations.
  Future<Option<state::Entry> > _get(const string& name);

  Future<bool> _set(const state::Entry& entry, const UUID& uuid);
  Future<bool> __set(const state::Entry& entry, const UUID& uuid);
  Future<bool> ___set(
      const state::Entry& entry,
      const Option<Log::Position>& position);

  Future<bool> _expunge(const state::Entry& entry);
  Future<bool> __expunge(const state::Entry& entry);
  Future<bool> ___expunge(
      const state::Entry& entry,
      const Option<Log::Position>& position);

  Future<std::set<string> > _names();

  Log::Reader reader;
  Log::Writer writer;

  // Used to serialize Log::Writer::append/truncate operations.
  Mutex mutex;

  // Whether or not we've started the ability to append to log.
  Option<Future<Nothing> > starting;

  // Last position in the log that we've read or written.
  Option<Log::Position> index;

  // Last position in the log up to which we've truncated.
  Option<Log::Position> truncated;

  // Note that while it would be nice to just use Operation::Snapshot
  // modified to include a required field called 'position' we don't
  // know the position (nor can we determine it) before we've done the
  // actual appending of the data.
  struct Snapshot
  {
    Snapshot(const Log::Position& position, const state::Entry& entry)
      : position(position), entry(entry) {}

    const Log::Position position;

    // TODO(benh): Rather than storing the entire state::Entry we
    // should just store the position, name, and UUID and cache the
    // data so we don't use too much memory.
    const state::Entry entry;
  };

  // All known snapshots indexed by name. Note that 'hashmap::get'
  // must be used instead of 'operator []' since Snapshot doesn't have
  // a default/empty constructor.
  hashmap<string, Snapshot> snapshots;
};


LogStorageProcess::LogStorageProcess(Log* log)
  : reader(log),
    writer(log) {}


LogStorageProcess::~LogStorageProcess() {}


void LogStorageProcess::finalize()
{
  if (starting.isSome()) {
    Future<Nothing>(starting.get()).discard();
  }
}


Future<Nothing> LogStorageProcess::start()
{
  if (starting.isSome()) {
    return starting.get();
  }

  starting = writer.start()
    .then(defer(self(), &Self::_start, lambda::_1));

  return starting.get();
}


Future<Nothing> LogStorageProcess::_start(
    const Option<Log::Position>& position)
{
  CHECK_SOME(starting);

  if (position.isNone()) {
    starting = None(); // Reset 'starting' so we try again.
    return start(); // TODO(benh): Don't try again forever?
  }

  // Now read and apply log entries. Since 'start' can be called
  // multiple times (i.e., since we reset 'starting' after getting a
  // None position returned after 'set', 'expunge', etc) we need to
  // check and see if we've already successfully read the log at least
  // once by checking 'index'. If we haven't yet read the log (i.e.,
  // this is the first call to 'start' and 'index' is None) then we
  // get the beginning of the log first so we can read from that up to
  // what ever position was known at the time we started the
  // writer. Note that it should always be safe to read a truncated
  // entry since a subsequent operation in the log should invalidate
  // that entry when we read it instead.
  if (index.isSome()) {
    // If we've started before (i.e., have an 'index' position) we
    // should also expect know the last 'truncated' position.
    CHECK_SOME(truncated);
    return reader.read(index.get(), position.get())
      .then(defer(self(), &Self::apply, lambda::_1));
  }

  return reader.beginning()
    .then(defer(self(), &Self::__start, lambda::_1, position.get()));
}


Future<Nothing> LogStorageProcess::__start(
    const Log::Position& beginning,
    const Log::Position& position)
{
  CHECK_SOME(starting);

  truncated = beginning; // Cache for future truncations.

  return reader.read(beginning, position)
    .then(defer(self(), &Self::apply, lambda::_1));
}


Future<Nothing> LogStorageProcess::apply(const list<Log::Entry>& entries)
{
  // Only read and apply entries past our index.
  foreach (const Log::Entry& entry, entries) {
    if (index.isNone() || index.get() < entry.position) {
      // Parse the Operation from the Log::Entry.
      Operation operation;

      google::protobuf::io::ArrayInputStream stream(
          entry.data.data(),
          entry.data.size());

      if (!operation.ParseFromZeroCopyStream(&stream)) {
        return Failure("Failed to deserialize Operation");
      }

      switch (operation.type()) {
        case Operation::SNAPSHOT: {
          CHECK(operation.has_snapshot());

          // Add or update the snapshot.
          Snapshot snapshot(entry.position, operation.snapshot().entry());
          snapshots.put(snapshot.entry.name(), snapshot);
          break;
        }

        case Operation::EXPUNGE: {
          CHECK(operation.has_expunge());
          snapshots.erase(operation.expunge().name());
          break;
        }

        default:
          return Failure("Unknown operation: " + stringify(operation.type()));
      }

      index = entry.position;
    }
  }

  return Nothing();
}


// TODO(benh): Truncation could be optimized by saving the "oldest"
// snapshot and only doing a truncation if/when we update that
// snapshot.
// TODO(benh): Truncation is not enough to keep the log size small as
// the log could get very fragmented. We'll need a way to defragment
// the log as some state entries might not get set over a long period
// of time and their associated snapshots are causing the log to grow
// very big.
void LogStorageProcess::truncate()
{
  // We lock the truncation since it includes a call to
  // Log::Writer::truncate which must be serialized with calls to
  // Log::Writer::append.
  mutex.lock()
    .then(defer(self(), &Self::_truncate))
    .onAny(lambda::bind(&Mutex::unlock, mutex));
}


Future<Nothing> LogStorageProcess::_truncate()
{
  // Determine the minimum necessary position for all the snapshots.
  Option<Log::Position> minimum = None();

  foreachvalue (const Snapshot& snapshot, snapshots) {
    minimum = min(minimum, snapshot.position);
  }

  CHECK_SOME(truncated);

  if (minimum.isSome() && minimum.get() > truncated.get()) {
    return writer.truncate(minimum.get())
      .then(defer(self(), &Self::__truncate, minimum.get(), lambda::_1));

    // NOTE: Any failure from Log::Writer::truncate doesn't propagate
    // since the expectation is any subsequent Log::Writer::append
    // would cause a failure. Furthermore, if the failure was
    // temporary any subsequent Log::Writer::truncate should rectify a
    // "missing" truncation.
  }

  return Nothing();
}


Future<Nothing> LogStorageProcess::__truncate(
    const Log::Position& minimum,
    const Option<Log::Position>& position)
{
  // Don't bother retrying truncation if we're demoted, we'll
  // just try again the next time 'truncate()' gets called
  // (after we've done what's necessary to append again).
  if (position.isSome()) {
    truncated = max(truncated, minimum);
    index = max(index, position);
  }

  return Nothing();
}


Future<Option<state::Entry> > LogStorageProcess::get(const string& name)
{
  return start()
    .then(defer(self(), &Self::_get, name));
}


Future<Option<state::Entry> > LogStorageProcess::_get(const string& name)
{
  Option<Snapshot> snapshot = snapshots.get(name);

  if (snapshot.isNone()) {
    return None();
  }

  return snapshot.get().entry;
}


Future<bool> LogStorageProcess::set(
    const state::Entry& entry,
    const UUID& uuid)
{
  return mutex.lock()
    .then(defer(self(), &Self::_set, entry, uuid))
    .onAny(lambda::bind(&Mutex::unlock, mutex));
}


Future<bool> LogStorageProcess::_set(
    const state::Entry& entry,
    const UUID& uuid)
{
  return start()
    .then(defer(self(), &Self::__set, entry, uuid));
}


Future<bool> LogStorageProcess::__set(
    const state::Entry& entry,
    const UUID& uuid)
{
  // Check the version first (if we've already got a snapshot).
  Option<Snapshot> snapshot = snapshots.get(entry.name());

  if (snapshot.isSome()) {
    if (UUID::fromBytes(snapshot.get().entry.uuid()) != uuid) {
      return false;
    }
  }

  // Now serialize and append a snapshot operation.
  Operation operation;
  operation.set_type(Operation::SNAPSHOT);
  operation.mutable_snapshot()->mutable_entry()->CopyFrom(entry);

  string value;
  if (!operation.SerializeToString(&value)) {
    return Failure("Failed to serialize Operation");
  }

  return writer.append(value)
    .then(defer(self(), &Self::___set, entry, lambda::_1));
}


Future<bool> LogStorageProcess::___set(
    const state::Entry& entry,
    const Option<Log::Position>& position)
{
  if (position.isNone()) {
    starting = None(); // Reset 'starting' so we try again.
    return false;
  }

  // Add (or update) the snapshot for this entry and truncate
  // the log if possible.
  CHECK(!snapshots.contains(entry.name()) ||
        snapshots.get(entry.name()).get().position < position.get());

  Snapshot snapshot(position.get(), entry);
  snapshots.put(snapshot.entry.name(), snapshot);
  truncate();

  // Update index so we don't bother with this position again.
  index = max(index, position);

  return true;
}


Future<bool> LogStorageProcess::expunge(const state::Entry& entry)
{
  return mutex.lock()
    .then(defer(self(), &Self::_expunge, entry))
    .onAny(lambda::bind(&Mutex::unlock, mutex));
}


Future<bool> LogStorageProcess::_expunge(const state::Entry& entry)
{
  return start()
    .then(defer(self(), &Self::__expunge, entry));
}


Future<bool> LogStorageProcess::__expunge(const state::Entry& entry)
{
  Option<Snapshot> snapshot = snapshots.get(entry.name());

  if (snapshot.isNone()) {
    return false;
  }

  // Check the version first.
  if (UUID::fromBytes(snapshot.get().entry.uuid()) !=
      UUID::fromBytes(entry.uuid())) {
    return false;
  }

  // Now serialize and append an expunge operation.
  Operation operation;
  operation.set_type(Operation::EXPUNGE);
  operation.mutable_expunge()->set_name(entry.name());

  string value;
  if (!operation.SerializeToString(&value)) {
    return Failure("Failed to serialize Operation");
  }

  return writer.append(value)
    .then(defer(self(), &Self::___expunge, entry, lambda::_1));
}


Future<bool> LogStorageProcess::___expunge(
    const state::Entry& entry,
    const Option<Log::Position>& position)
{
  if (position.isNone()) {
    starting = None(); // Reset 'starting' so we try again.
    return false;
  }

  // Remove from snapshots and truncate the log if possible.
  CHECK(snapshots.contains(entry.name()));
  snapshots.erase(entry.name());
  truncate();

  return true;
}


Future<std::set<string> > LogStorageProcess::names()
{
  return start()
    .then(defer(self(), &Self::_names));
}


Future<std::set<string> > LogStorageProcess::_names()
{
  const hashset<string>& keys = snapshots.keys();
  return std::set<string>(keys.begin(), keys.end());
}


LogStorage::LogStorage(Log* log)
{
  process = new LogStorageProcess(log);
  spawn(process);
}


LogStorage::~LogStorage()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<state::Entry> > LogStorage::get(const string& name)
{
  return dispatch(process, &LogStorageProcess::get, name);
}


Future<bool> LogStorage::set(const state::Entry& entry, const UUID& uuid)
{
  return dispatch(process, &LogStorageProcess::set, entry, uuid);
}


Future<bool> LogStorage::expunge(const state::Entry& entry)
{
  return dispatch(process, &LogStorageProcess::expunge, entry);
}


Future<std::set<string> > LogStorage::names()
{
  return dispatch(process, &LogStorageProcess::names);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
