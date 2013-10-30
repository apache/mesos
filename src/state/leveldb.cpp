#include <leveldb/db.h>

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "state/leveldb.hpp"
#include "state/storage.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace state {

LevelDBStorageProcess::LevelDBStorageProcess(const string& _path)
  : path(_path), db(NULL) {}


LevelDBStorageProcess::~LevelDBStorageProcess()
{
  delete db; // NULL if open failed in LevelDBStorageProcess::initialize.
}


void LevelDBStorageProcess::initialize()
{
  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::Status status = leveldb::DB::Open(options, path, &db);

  if (!status.ok()) {
    // TODO(benh): Consider trying to repair the DB.
    error = Option<string>::some(status.ToString());
  } else {
    // TODO(benh): Conditionally compact to avoid long recovery times?
    db->CompactRange(NULL, NULL);
  }
}


Future<vector<string> > LevelDBStorageProcess::names()
{
  if (error.isSome()) {
    return Future<vector<string> >::failed(error.get());
  }

  vector<string> results;

  leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());

  iterator->SeekToFirst();

  while (iterator->Valid()) {
    results.push_back(iterator->key().ToString());
    iterator->Next();
  }

  delete iterator;

  return results;
}


Future<Option<Entry> > LevelDBStorageProcess::get(const string& name)
{
  if (error.isSome()) {
    return Future<Option<Entry> >::failed(error.get());
  }

  Try<Option<Entry> > option = read(name);

  if (option.isError()) {
    return Future<Option<Entry> >::failed(option.error());
  }

  return option.get();
}


Future<bool> LevelDBStorageProcess::set(const Entry& entry, const UUID& uuid)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  // We do a read first to make sure the version has not changed. This
  // could be optimized in the future, for now it will probably hit
  // the cache anyway.
  Try<Option<Entry> > option = read(entry.name());

  if (option.isError()) {
    return Future<bool>::failed(option.error());
  }

  if (option.get().isSome()) {
    if (UUID::fromBytes(option.get().get().uuid()) != uuid) {
      return false;
    }
  }

  // Note that the read (i.e., DB::Get) and the write (i.e., DB::Put)
  // are inherently "atomic" because only one db can be opened at a
  // time, so there can not be any writes that occur concurrently.

  Try<bool> result = write(entry);

  if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  return result.get();
}


Future<bool> LevelDBStorageProcess::expunge(const Entry& entry)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  // We do a read first to make sure the version has not changed. This
  // could be optimized in the future, for now it will probably hit
  // the cache anyway.
  Try<Option<Entry> > option = read(entry.name());

  if (option.isError()) {
    return Future<bool>::failed(option.error());
  }

  if (option.get().isNone()) {
    return false;
  }

  if (UUID::fromBytes(option.get().get().uuid()) !=
      UUID::fromBytes(entry.uuid())) {
    return false;
  }

  // Note that the read (i.e., DB::Get) and DB::Delete are inherently
  // "atomic" because only one db can be opened at a time, so there
  // can not be any writes that occur concurrently.

  leveldb::WriteOptions options;
  options.sync = true;

  leveldb::Status status = db->Delete(options, entry.name());

  if (!status.ok()) {
    return Future<bool>::failed(status.ToString());
  }

  return true;
}


Try<Option<Entry> > LevelDBStorageProcess::read(const string& name)
{
  CHECK(error.isNone());

  leveldb::ReadOptions options;

  string value;

  leveldb::Status status = db->Get(options, name, &value);

  if (status.IsNotFound()) {
    return None();
  } else if (!status.ok()) {
    return Error(status.ToString());
  }

  google::protobuf::io::ArrayInputStream stream(value.data(), value.size());

  Entry entry;

  if (!entry.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize Entry");
  }

  return Option<Entry>::some(entry);
}


Try<bool> LevelDBStorageProcess::write(const Entry& entry)
{
  CHECK(error.isNone());

  leveldb::WriteOptions options;
  options.sync = true;

  string value;

  if (!entry.SerializeToString(&value)) {
    return Error("Failed to serialize Entry");
  }

  leveldb::Status status = db->Put(options, entry.name(), value);

  if (!status.ok()) {
    return Error(status.ToString());
  }

  return true;
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
