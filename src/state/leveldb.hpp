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

#include "state/serializer.hpp"
#include "state/state.hpp"

// Forward declarations.
namespace leveldb { class DB; }


namespace mesos {
namespace internal {
namespace state {

// More forward declarations.
class LevelDBStateProcess;


template <typename Serializer = StringSerializer>
class LevelDBState : public State<Serializer>
{
public:
  LevelDBState(const std::string& path);
  virtual ~LevelDBState();

  // State implementation.
  virtual process::Future<std::vector<std::string> > names();

protected:
  // More State implementation.
  virtual process::Future<Option<Entry> > fetch(const std::string& name);
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid);

private:
  LevelDBStateProcess* process;
};


class LevelDBStateProcess : public process::Process<LevelDBStateProcess>
{
public:
  LevelDBStateProcess(const std::string& path);
  virtual ~LevelDBStateProcess();

  virtual void initialize();

  // State implementation.
  process::Future<std::vector<std::string> > names();
  process::Future<Option<Entry> > fetch(const std::string& name);
  process::Future<bool> swap(const Entry& entry, const UUID& uuid);

private:
  // Helpers for interacting with leveldb.
  Try<Option<Entry> > get(const std::string& name);
  Try<bool> put(const Entry& entry);

  const std::string path;
  leveldb::DB* db;

  Option<std::string> error;
};


template <typename Serializer>
LevelDBState<Serializer>::LevelDBState(const std::string& path)
{
  process = new LevelDBStateProcess(path);
  process::spawn(process);
}


template <typename Serializer>
LevelDBState<Serializer>::~LevelDBState()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


template <typename Serializer>
process::Future<std::vector<std::string> > LevelDBState<Serializer>::names()
{
  return process::dispatch(process, &LevelDBStateProcess::names);
}


template <typename Serializer>
process::Future<Option<Entry> > LevelDBState<Serializer>::fetch(
    const std::string& name)
{
  return process::dispatch(process, &LevelDBStateProcess::fetch, name);
}


template <typename Serializer>
process::Future<bool> LevelDBState<Serializer>::swap(
    const Entry& entry,
    const UUID& uuid)
{
  return process::dispatch(process, &LevelDBStateProcess::swap, entry, uuid);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_LEVELDB_HPP__
