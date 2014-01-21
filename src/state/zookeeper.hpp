#ifndef __STATE_ZOOKEEPER_HPP__
#define __STATE_ZOOKEEPER_HPP__

#include <queue>
#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/state.hpp"

#include "state/storage.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

namespace mesos {
namespace internal {
namespace state {

// Forward declarations.
class ZooKeeperStorageProcess;


class ZooKeeperStorage : public Storage
{
public:
  // TODO(benh): Just take a zookeeper::URL.
  ZooKeeperStorage(
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth = None());
  virtual ~ZooKeeperStorage();

  // Storage implementation.
  virtual process::Future<Option<Entry> > get(const std::string& name);
  virtual process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  virtual process::Future<std::vector<std::string> > names();

private:
  ZooKeeperStorageProcess* process;
};


class ZooKeeperStorageProcess : public process::Process<ZooKeeperStorageProcess>
{
public:
  ZooKeeperStorageProcess(
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth);
  virtual ~ZooKeeperStorageProcess();

  virtual void initialize();

  // Storage implementation.
  process::Future<Option<Entry> > get(const std::string& name);
  process::Future<bool> set(const Entry& entry, const UUID& uuid);
  virtual process::Future<bool> expunge(const Entry& entry);
  process::Future<std::vector<std::string> > names();

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const std::string& path);
  void created(const std::string& path);
  void deleted(const std::string& path);

private:
  // Helpers for getting the names, fetching, and swapping.
  Result<std::vector<std::string> > doNames();
  Result<Option<Entry> > doGet(const std::string& name);
  Result<bool> doSet(const Entry& entry, const UUID& uuid);
  Result<bool> doExpunge(const Entry& entry);

  const std::string servers;

  // The session timeout requested by the client.
  const Duration timeout;

  const std::string znode;

  Option<zookeeper::Authentication> auth; // ZooKeeper authentication.

  const ACL_vector acl; // Default ACL to use.

  Watcher* watcher;
  ZooKeeper* zk;

  enum State { // ZooKeeper connection state.
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

  struct Names
  {
    process::Promise<std::vector<std::string> > promise;
  };

  struct Get
  {
    Get(const std::string& _name)
      : name(_name) {}
    std::string name;
    process::Promise<Option<Entry> > promise;
  };

  struct Set
  {
    Set(const Entry& _entry, const UUID& _uuid)
      : entry(_entry), uuid(_uuid) {}
    Entry entry;
    UUID uuid;
    process::Promise<bool> promise;
  };

  struct Expunge
  {
    Expunge(const Entry& _entry)
      : entry(_entry) {}
    Entry entry;
    process::Promise<bool> promise;
  };

  // TODO(benh): Make pending a single queue of "operations" that can
  // be "invoked" (C++11 lambdas would help).
  struct {
    std::queue<Names*> names;
    std::queue<Get*> gets;
    std::queue<Set*> sets;
    std::queue<Expunge*> expunges;
  } pending;

  Option<std::string> error;
};


inline ZooKeeperStorage::ZooKeeperStorage(
    const std::string& servers,
    const Duration& timeout,
    const std::string& znode,
    const Option<zookeeper::Authentication>& auth)
{
  process = new ZooKeeperStorageProcess(servers, timeout, znode, auth);
  process::spawn(process);
}


inline ZooKeeperStorage::~ZooKeeperStorage()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


inline process::Future<Option<Entry> > ZooKeeperStorage::get(
    const std::string& name)
{
  return process::dispatch(process, &ZooKeeperStorageProcess::get, name);
}


inline process::Future<bool> ZooKeeperStorage::set(
    const Entry& entry,
    const UUID& uuid)
{
  return process::dispatch(process, &ZooKeeperStorageProcess::set, entry, uuid);
}


inline process::Future<bool> ZooKeeperStorage::expunge(
    const Entry& entry)
{
  return process::dispatch(process, &ZooKeeperStorageProcess::expunge, entry);
}


inline process::Future<std::vector<std::string> > ZooKeeperStorage::names()
{
  return process::dispatch(process, &ZooKeeperStorageProcess::names);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_ZOOKEEPER_HPP__
