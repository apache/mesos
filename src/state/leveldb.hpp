#ifndef __STATE_LEVELDB_HPP__
#define __STATE_LEVELDB_HPP__

#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

// Forward declarations.
namespace leveldb { class DB; }


namespace mesos {
namespace internal {
namespace state {

// More forward declarations.
class LevelDBStorageProcess;


class LevelDBStorage : public Storage
{
public:
  LevelDBStorage(const std::string& path);
  virtual ~LevelDBStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::vector<std::string> > names();

private:
  LevelDBStorageProcess* process;
};


class LevelDBStorageProcess : public process::Process<LevelDBStorageProcess>
{
public:
  LevelDBStorageProcess(const std::string& path);
  virtual ~LevelDBStorageProcess();

  virtual void initialize();

  // Storage implementation.
  process::Future<Option<Entry> > get(const std::string& name);
  process::Future<bool> set(const Entry& entry, const UUID& uuid);
  process::Future<bool> expunge(const Entry& entry);
  process::Future<std::vector<std::string> > names();

private:
  // Helpers for interacting with leveldb.
  Try<Option<Entry> > read(const std::string& name);
  Try<bool> write(const Entry& entry);

  const std::string path;
  leveldb::DB* db;

  Option<std::string> error;
};


inline LevelDBStorage::LevelDBStorage(const std::string& path)
{
  process = new LevelDBStorageProcess(path);
  process::spawn(process);
}


inline LevelDBStorage::~LevelDBStorage()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


inline process::Future<Option<Entry> > LevelDBStorage::get(
    const std::string& name)
{
  return process::dispatch(process, &LevelDBStorageProcess::get, name);
}


inline process::Future<bool> LevelDBStorage::set(
    const Entry& entry,
    const UUID& uuid)
{
  return process::dispatch(process, &LevelDBStorageProcess::set, entry, uuid);
}


inline process::Future<bool> LevelDBStorage::expunge(
    const Entry& entry)
{
  return process::dispatch(process, &LevelDBStorageProcess::expunge, entry);
}


inline process::Future<std::vector<std::string> > LevelDBStorage::names()
{
  return process::dispatch(process, &LevelDBStorageProcess::names);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_LEVELDB_HPP__
