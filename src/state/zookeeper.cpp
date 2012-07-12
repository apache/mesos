#include <google/protobuf/message.h>

#include <queue>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/time.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "state/state.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::queue;
using std::string;

using zookeeper::Authentication;

namespace mesos {
namespace internal {
namespace state {

class ZooKeeperStateProcess : public Process<ZooKeeperStateProcess>
{
public:
  ZooKeeperStateProcess(
      const string& servers,
      const seconds& timeout,
      const string& znode,
      const Option<Authentication>& auth);
  virtual ~ZooKeeperStateProcess();

  virtual void initialize();

  // State implementation.
  Future<Option<Entry> > fetch(const string& name);
  Future<bool> swap(const Entry& entry, const UUID& uuid);

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const string& path);
  void created(const string& path);
  void deleted(const string& path);

private:
  // Helpers for fetching and swapping.
  Result<Option<Entry> > doFetch(const string& name);
  Result<bool> doSwap(const Entry& entry, const UUID& uuid);

  const string servers;
  const seconds timeout;
  const string znode;

  Option<Authentication> auth; // ZooKeeper authentication.

  const ACL_vector acl; // Default ACL to use.

  Watcher* watcher;
  ZooKeeper* zk;

  enum State { // ZooKeeper connection state.
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

  struct Fetch
  {
    Fetch(const string& _name)
      : name(_name) {}
    string name;
    Promise<Option<Entry> > promise;
  };

  struct Swap
  {
    Swap(const Entry& _entry, const UUID& _uuid)
      : entry(_entry), uuid(_uuid) {}
    Entry entry;
    UUID uuid;
    Promise<bool> promise;
  };

  struct {
    queue<Fetch*> fetches;
    queue<Swap*> swaps;
  } pending;

  Option<string> error;
};


// Helper for failing a queue of promises.
template <typename T>
void fail(queue<T*>* queue, const string& message)
{
  while (!queue->empty()) {
    T* t = queue->front();
    queue->pop();
    t->promise.fail(message);
    delete t;
  }
}


ZooKeeperStateProcess::ZooKeeperStateProcess(
    const string& _servers,
    const seconds& _timeout,
    const string& _znode,
    const Option<Authentication>& _auth)
  : servers(_servers),
    timeout(_timeout),
    znode(strings::remove(_znode, "/", strings::SUFFIX)),
    auth(_auth),
    acl(_auth.isSome()
        ? zookeeper::EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    watcher(NULL),
    zk(NULL),
    state(DISCONNECTED)
{}


ZooKeeperStateProcess::~ZooKeeperStateProcess()
{
  fail(&pending.fetches, "No longer managing state");
  fail(&pending.swaps, "No longer managing state");

  delete zk;
  delete watcher;
}


void ZooKeeperStateProcess::initialize()
{
  // Doing initialization here allows to avoid the race between
  // instantiating the ZooKeeper instance and being spawned ourself.
  watcher = new ProcessWatcher<ZooKeeperStateProcess>(self());
  zk = new ZooKeeper(servers, timeout, watcher);
}


Future<Option<Entry> > ZooKeeperStateProcess::fetch(const string& name)
{
  if (error.isSome()) {
    return Future<Option<Entry> >::failed(error.get());
  } else if (state != CONNECTED) {
    Fetch* fetch = new Fetch(name);
    pending.fetches.push(fetch);
    return fetch->promise.future();
  }

  Result<Option<Entry> > result = doFetch(name);

  if (result.isNone()) { // Try again later.
    Fetch* fetch = new Fetch(name);
    pending.fetches.push(fetch);
    return fetch->promise.future();
  } else if (result.isError()) {
    return Future<Option<Entry> >::failed(result.error());
  }

  return result.get();
}


Future<bool> ZooKeeperStateProcess::swap(const Entry& entry, const UUID& uuid)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  } else if (state != CONNECTED) {
    Swap* swap = new Swap(entry, uuid);
    pending.swaps.push(swap);
    return swap->promise.future();
  }

  Result<bool> result = doSwap(entry, uuid);

  if (result.isNone()) { // Try again later.
    Swap* swap = new Swap(entry, uuid);
    pending.swaps.push(swap);
    return swap->promise.future();
  } else if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  return result.get();
}


void ZooKeeperStateProcess::connected(bool reconnect)
{
  if (!reconnect) {
    // Authenticate if necessary (and we are connected for the first
    // time, or after a session expiration).
    if (auth.isSome()) {
      LOG(INFO) << "Authenticating with ZooKeeper using " << auth.get().scheme;

      int code = zk->authenticate(auth.get().scheme, auth.get().credentials);

      if (code != ZOK) { // TODO(benh): Authentication retries?
        Try<string> message = strings::format(
            "Failed to authenticate with ZooKeeper: %s", zk->message(code));
        error = message.isSome()
          ? message.get()
          : "Failed to authenticate with ZooKeeper";
        return;
      }
    }
  }

  state = CONNECTED;

  while (!pending.fetches.empty()) {
    Fetch* fetch = pending.fetches.front();
    Result<Option<Entry> > result = doFetch(fetch->name);
    if (result.isNone()) {
      return; // Try again later.
    } else if (result.isError()) {
      fetch->promise.fail(result.error());
    } else {
      fetch->promise.set(result.get());
    }
    pending.fetches.pop();
    delete fetch;
  }

  while (!pending.swaps.empty()) {
    Swap* swap = pending.swaps.front();
    Result<bool> result = doSwap(swap->entry, swap->uuid);
    if (result.isNone()) {
      return; // Try again later.
    } else if (result.isError()) {
      swap->promise.fail(result.error());
    } else {
      swap->promise.set(result.get());
    }
    pending.swaps.pop();
    delete swap;
  }
}


void ZooKeeperStateProcess::reconnecting()
{
  state = CONNECTING;
}


void ZooKeeperStateProcess::expired()
{
  state = DISCONNECTED;

  delete zk;
  zk = new ZooKeeper(servers, timeout, watcher);

  state = CONNECTING;
}


void ZooKeeperStateProcess::updated(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStateProcess::created(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStateProcess::deleted(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


Result<Option<Entry> > ZooKeeperStateProcess::doFetch(const string& name)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  string result;
  Stat stat;

  int code = zk->get(znode + "/" + name, false, &result, &stat);

  if (code == ZNONODE) {
    return Option<Entry>::none();
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return Result<Option<Entry> >::none();
  } else if (code != ZOK) {
    return Result<Option<Entry> >::error(
        "Failed to get '" + znode + "/" + name +
        "' in ZooKeeper: " + zk->message(code));
  }

  google::protobuf::io::ArrayInputStream stream(result.data(), result.size());

  Entry entry;

  if (!entry.ParseFromZeroCopyStream(&stream)) {
    return Result<Option<Entry> >::error("Failed to deserialize Entry");
  }

  return Option<Entry>::some(entry);
}


Result<bool> ZooKeeperStateProcess::doSwap(const Entry& entry, const UUID& uuid)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  // Serialize to make sure we're under the 1 MB limit.
  string data;

  if (!entry.SerializeToString(&data)) {
    return Result<bool>::error("Failed to serialize Entry");
  }

  if (data.size() > 1024 * 1024) { // 1 MB
    // TODO(benh): Implement compression.
    return Result<bool>::error("Serialized data is too big (> 1 MB)");
  }

  string result;
  Stat stat;

  int code = zk->get(znode + "/" + entry.name(), false, &result, &stat);

  if (code == ZNONODE) {
    // Create directory path znodes as necessary.
    CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');
    size_t index = znode.find("/", 0);

    while (index < string::npos) {
      // Get out the prefix to create.
      index = znode.find("/", index + 1);
      string prefix = znode.substr(0, index);

      // Create the znode (even if it already exists).
      code = zk->create(prefix, "", acl, 0, NULL);

      if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
        CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
        return Result<bool>::none();
      } else if (code != ZOK && code != ZNODEEXISTS) {
        return Result<bool>::error(
            "Failed to create '" + prefix +
            "' in ZooKeeper: " + zk->message(code));
      }
    }

    code = zk->create(znode + "/" + entry.name(), data, acl, 0, NULL);

    if (code == ZNODEEXISTS) {
      return false; // Lost a race with someone else.
    } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
      CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
      return Result<bool>::none();
    } else if (code != ZOK) {
      return Result<bool>::error(
          "Failed to create '" + znode + "/" + entry.name() +
          "' in ZooKeeper: " + zk->message(code));
    }

    return true;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return Result<bool>::none();
  } else if (code != ZOK) {
    return Result<bool>::error(
        "Failed to get '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  google::protobuf::io::ArrayInputStream stream(result.data(), result.size());

  Entry current;

  if (!current.ParseFromZeroCopyStream(&stream)) {
    return Result<bool>::error("Failed to deserialize Entry");
  }

  if (UUID::fromBytes(current.uuid()) != uuid) {
    return false;
  }

  // Okay, do a set, we get atomic swap by requiring 'stat.version'.
  code = zk->set(znode + "/" + entry.name(), data, stat.version);

  if (code == ZBADVERSION) {
    return false;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return Result<bool>::none();
  } else if (code != ZOK) {
    return Result<bool>::error(
        "Failed to set '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  return true;
}


ZooKeeperState::ZooKeeperState(
    const string& servers,
    const seconds& timeout,
    const string& znode,
    const Option<Authentication>& auth)
{
  process = new ZooKeeperStateProcess(servers, timeout, znode, auth);
  spawn(process);
}


ZooKeeperState::~ZooKeeperState()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<Entry> > ZooKeeperState::fetch(const string& name)
{
  return dispatch(process, &ZooKeeperStateProcess::fetch, name);
}


Future<bool> ZooKeeperState::swap(const Entry& entry, const UUID& uuid)
{
  return dispatch(process, &ZooKeeperStateProcess::swap, entry, uuid);
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
