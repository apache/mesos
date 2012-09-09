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

#include "state/serializer.hpp"
#include "state/state.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

namespace mesos {
namespace internal {
namespace state {

// Forward declarations.
class ZooKeeperStateProcess;


template <typename Serializer = StringSerializer>
class ZooKeeperState : public State<Serializer>
{
public:
  // TODO(benh): Just take a zookeeper::URL.
  ZooKeeperState(
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth =
      Option<zookeeper::Authentication>());
  virtual ~ZooKeeperState();

  // State implementation.
  virtual process::Future<std::vector<std::string> > names();

protected:
  // More State implementation.
  virtual process::Future<Option<Entry> > fetch(const std::string& name);
  virtual process::Future<bool> swap(const Entry& entry, const UUID& uuid);

private:
  ZooKeeperStateProcess* process;
};


class ZooKeeperStateProcess : public process::Process<ZooKeeperStateProcess>
{
public:
  ZooKeeperStateProcess(
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth);
  virtual ~ZooKeeperStateProcess();

  virtual void initialize();

  // State implementation.
  process::Future<std::vector<std::string> > names();
  process::Future<Option<Entry> > fetch(const std::string& name);
  process::Future<bool> swap(const Entry& entry, const UUID& uuid);

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
  Result<Option<Entry> > doFetch(const std::string& name);
  Result<bool> doSwap(const Entry& entry, const UUID& uuid);

  const std::string servers;
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

  struct Fetch
  {
    Fetch(const std::string& _name)
      : name(_name) {}
    std::string name;
    process::Promise<Option<Entry> > promise;
  };

  struct Swap
  {
    Swap(const Entry& _entry, const UUID& _uuid)
      : entry(_entry), uuid(_uuid) {}
    Entry entry;
    UUID uuid;
    process::Promise<bool> promise;
  };

  // TODO(benh): Make pending a single queue of "operations" that can
  // be "invoked" (C++11 lambdas would help).
  struct {
    std::queue<Names*> names;
    std::queue<Fetch*> fetches;
    std::queue<Swap*> swaps;
  } pending;

  Option<std::string> error;
};


template <typename Serializer>
ZooKeeperState<Serializer>::ZooKeeperState(
    const std::string& servers,
    const Duration& timeout,
    const std::string& znode,
    const Option<zookeeper::Authentication>& auth)
{
  process = new ZooKeeperStateProcess(servers, timeout, znode, auth);
  process::spawn(process);
}


template <typename Serializer>
ZooKeeperState<Serializer>::~ZooKeeperState()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


template <typename Serializer>
process::Future<std::vector<std::string> > ZooKeeperState<Serializer>::names()
{
  return process::dispatch(process, &ZooKeeperStateProcess::names);
}


template <typename Serializer>
process::Future<Option<Entry> > ZooKeeperState<Serializer>::fetch(
    const std::string& name)
{
  return process::dispatch(process, &ZooKeeperStateProcess::fetch, name);
}


template <typename Serializer>
process::Future<bool> ZooKeeperState<Serializer>::swap(
    const Entry& entry,
    const UUID& uuid)
{
  return process::dispatch(process, &ZooKeeperStateProcess::swap, entry, uuid);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_ZOOKEEPER_HPP__
