#include <set>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/in_memory.hpp"
#include "state/storage.hpp"

using namespace process;

// Note that we don't add 'using std::set' here because we need
// 'std::' to disambiguate the 'set' member.
using std::string;

namespace mesos {
namespace internal {
namespace state {


class InMemoryStorageProcess : public Process<InMemoryStorageProcess>
{
public:
  Option<Entry> get(const string& name)
  {
    return entries.get(name);
  }

  bool set(const Entry& entry, const UUID& uuid)
  {
    const Option<Entry>& option = entries.get(entry.name());

    if (option.isSome() && UUID::fromBytes(option.get().uuid()) != uuid) {
      return false;
    }

    entries.put(entry.name(), entry);
    return true;
  }

  bool expunge(const Entry& entry)
  {
    const Option<Entry>& option = entries.get(entry.name());

    if (option.isNone()) {
      return false;
    }

    if (UUID::fromBytes(option.get().uuid()) != UUID::fromBytes(entry.uuid())) {
      return false;
    }

    entries.erase(entry.name());
    return true;
  }

  std::set<string> names() // Use std:: to disambiguate 'set' member.
  {
    const hashset<string>& keys = entries.keys();
    return std::set<string>(keys.begin(), keys.end());
  }

private:
  hashmap<string, Entry> entries;
};



InMemoryStorage::InMemoryStorage()
{
  process = new InMemoryStorageProcess();
  spawn(process);
}


InMemoryStorage::~InMemoryStorage()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<Entry> > InMemoryStorage::get(const string& name)
{
  return dispatch(process, &InMemoryStorageProcess::get, name);
}


Future<bool> InMemoryStorage::set(const Entry& entry, const UUID& uuid)
{
  return dispatch(process, &InMemoryStorageProcess::set, entry, uuid);
}


Future<bool> InMemoryStorage::expunge(const Entry& entry)
{
  return dispatch(process, &InMemoryStorageProcess::expunge, entry);
}


Future<std::set<string> > InMemoryStorage::names()
{
  return dispatch(process, &InMemoryStorageProcess::names);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
