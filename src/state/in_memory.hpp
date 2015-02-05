#ifndef __STATE_IN_MEMORY_HPP__
#define __STATE_IN_MEMORY_HPP__

#include <set>
#include <string>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

namespace mesos {
namespace state {

// Forward declaration.
class InMemoryStorageProcess;


class InMemoryStorage : public Storage
{
public:
  InMemoryStorage();
  virtual ~InMemoryStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::set<std::string> > names();

private:
  InMemoryStorageProcess* process;
};

} // namespace state {
} // namespace mesos {

#endif // __STATE_IN_MEMORY_HPP__
