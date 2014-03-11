#ifndef __STATE_LEVELDB_HPP__
#define __STATE_LEVELDB_HPP__

#include <string>
#include <vector>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

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

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_LEVELDB_HPP__
