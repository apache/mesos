#ifndef __STATE_LOG_HPP__
#define __STATE_LOG_HPP__

#include <set>
#include <string>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "log/log.hpp"

#include "messages/state.hpp"

#include "state/storage.hpp"

namespace mesos {
namespace state {

// Forward declarations.
class LogStorageProcess;


class LogStorage : public Storage
{
public:
  LogStorage(log::Log* log, size_t diffsBetweenSnapshots = 0);

  virtual ~LogStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::set<std::string> > names();

private:
  LogStorageProcess* process;
};

} // namespace state {
} // namespace mesos {

#endif // __STATE_LOG_HPP__
