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
// limitations under the License

#include <leveldb/db.h>

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <set>
#include <string>

#include <mesos/state/leveldb.hpp>
#include <mesos/state/storage.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

using namespace process;

// Note that we don't add 'using std::set' here because we need
// 'std::' to disambiguate the 'set' member.
using std::string;

using mesos::internal::state::Entry;

namespace mesos {
namespace state {


class LevelDBStorageProcess : public Process<LevelDBStorageProcess>
{
public:
  explicit LevelDBStorageProcess(const string& path);
  ~LevelDBStorageProcess() override;

  void initialize() override;

  // Storage implementation.
  Future<Option<Entry>> get(const string& name);
  Future<bool> set(const Entry& entry, const id::UUID& uuid);
  Future<bool> expunge(const Entry& entry);
  Future<std::set<string>> names();

private:
  // Helpers for interacting with leveldb.
  Try<Option<Entry>> read(const string& name);
  Try<bool> write(const Entry& entry);

  const string path;
  leveldb::DB* db;

  Option<string> error;
};


LevelDBStorageProcess::LevelDBStorageProcess(const string& _path)
  : ProcessBase(process::ID::generate("leveldb-storage")),
    path(_path),
    db(nullptr) {}


LevelDBStorageProcess::~LevelDBStorageProcess()
{
  delete db; // nullptr if open failed in LevelDBStorageProcess::initialize.
}


void LevelDBStorageProcess::initialize()
{
  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::Status status = leveldb::DB::Open(options, path, &db);

  if (!status.ok()) {
    // TODO(benh): Consider trying to repair the DB.
    error = status.ToString();
  } else {
    // TODO(benh): Conditionally compact to avoid long recovery times?
    db->CompactRange(nullptr, nullptr);
  }
}


Future<std::set<string>> LevelDBStorageProcess::names()
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  std::set<string> results;

  leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());

  iterator->SeekToFirst();

  while (iterator->Valid()) {
    results.insert(iterator->key().ToString());
    iterator->Next();
  }

  delete iterator;

  return results;
}


Future<Option<Entry>> LevelDBStorageProcess::get(const string& name)
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  Try<Option<Entry>> option = read(name);

  if (option.isError()) {
    return Failure(option.error());
  }

  return option.get();
}


Future<bool> LevelDBStorageProcess::set(
    const Entry& entry,
    const id::UUID& uuid)
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  // We do a read first to make sure the version has not changed. This
  // could be optimized in the future, for now it will probably hit
  // the cache anyway.
  Try<Option<Entry>> option = read(entry.name());

  if (option.isError()) {
    return Failure(option.error());
  }

  if (option->isSome()) {
    if (id::UUID::fromBytes(option.get()->uuid()).get() != uuid) {
      return false;
    }
  }

  // Note that the read (i.e., DB::Get) and the write (i.e., DB::Put)
  // are inherently "atomic" because only one db can be opened at a
  // time, so there cannot be any writes that occur concurrently.

  Try<bool> result = write(entry);

  if (result.isError()) {
    return Failure(result.error());
  }

  return result.get();
}


Future<bool> LevelDBStorageProcess::expunge(const Entry& entry)
{
  if (error.isSome()) {
    return Failure(error.get());
  }

  // We do a read first to make sure the version has not changed. This
  // could be optimized in the future, for now it will probably hit
  // the cache anyway.
  Try<Option<Entry>> option = read(entry.name());

  if (option.isError()) {
    return Failure(option.error());
  }

  if (option->isNone()) {
    return false;
  }

  if (id::UUID::fromBytes(option.get()->uuid()).get() !=
      id::UUID::fromBytes(entry.uuid()).get()) {
    return false;
  }

  // Note that the read (i.e., DB::Get) and DB::Delete are inherently
  // "atomic" because only one db can be opened at a time, so there
  // cannot be any writes that occur concurrently.

  leveldb::WriteOptions options;
  options.sync = true;

  leveldb::Status status = db->Delete(options, entry.name());

  if (!status.ok()) {
    return Failure(status.ToString());
  }

  return true;
}


Try<Option<Entry>> LevelDBStorageProcess::read(const string& name)
{
  CHECK_NONE(error);

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

  return Some(entry);
}


Try<bool> LevelDBStorageProcess::write(const Entry& entry)
{
  CHECK_NONE(error);

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


LevelDBStorage::LevelDBStorage(const string& path)
{
  process = new LevelDBStorageProcess(path);
  spawn(process);
}


LevelDBStorage::~LevelDBStorage()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<Entry>> LevelDBStorage::get(const string& name)
{
  return dispatch(process, &LevelDBStorageProcess::get, name);
}


Future<bool> LevelDBStorage::set(const Entry& entry, const id::UUID& uuid)
{
  return dispatch(process, &LevelDBStorageProcess::set, entry, uuid);
}


Future<bool> LevelDBStorage::expunge(const Entry& entry)
{
  return dispatch(process, &LevelDBStorageProcess::expunge, entry);
}


Future<std::set<string>> LevelDBStorage::names()
{
  return dispatch(process, &LevelDBStorageProcess::names);
}

} // namespace state {
} // namespace mesos {
