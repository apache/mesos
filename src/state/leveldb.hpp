#ifndef __STATE_LEVELDB_HPP__
#define __STATE_LEVELDB_HPP__

#include <set>
#include <string>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

namespace mesos {
namespace state {

// More forward declarations.
class LevelDBStorageProcess;


class LevelDBStorage : public Storage
{
public:
  explicit LevelDBStorage(const std::string& path);
  virtual ~LevelDBStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::set<std::string> > names();

private:
  LevelDBStorageProcess* process;
};

} // namespace state {
} // namespace mesos {

#endif // __STATE_LEVELDB_HPP__
