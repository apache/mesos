#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <queue>
#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "state/storage.hpp"
#include "state/zookeeper.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

using namespace process;

using std::queue;
using std::string;
using std::vector;

using zookeeper::Authentication;

namespace mesos {
namespace internal {
namespace state {

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


ZooKeeperStorageProcess::ZooKeeperStorageProcess(
    const string& _servers,
    const Duration& _timeout,
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


ZooKeeperStorageProcess::~ZooKeeperStorageProcess()
{
  fail(&pending.names, "No longer managing storage");
  fail(&pending.gets, "No longer managing storage");
  fail(&pending.sets, "No longer managing storage");

  delete zk;
  delete watcher;
}


void ZooKeeperStorageProcess::initialize()
{
  // Doing initialization here allows to avoid the race between
  // instantiating the ZooKeeper instance and being spawned ourself.
  watcher = new ProcessWatcher<ZooKeeperStorageProcess>(self());
  zk = new ZooKeeper(servers, timeout, watcher);
}


Future<vector<string> > ZooKeeperStorageProcess::names()
{
  if (error.isSome()) {
    return Future<vector<string> >::failed(error.get());
  } else if (state != CONNECTED) {
    Names* names = new Names();
    pending.names.push(names);
    return names->promise.future();
  }

  Result<vector<string> > result = doNames();

  if (result.isNone()) { // Try again later.
    Names* names = new Names();
    pending.names.push(names);
    return names->promise.future();
  } else if (result.isError()) {
    return Future<vector<string> >::failed(result.error());
  }

  return result.get();
}


Future<Option<Entry> > ZooKeeperStorageProcess::get(const string& name)
{
  if (error.isSome()) {
    return Future<Option<Entry> >::failed(error.get());
  } else if (state != CONNECTED) {
    Get* get = new Get(name);
    pending.gets.push(get);
    return get->promise.future();
  }

  Result<Option<Entry> > result = doGet(name);

  if (result.isNone()) { // Try again later.
    Get* get = new Get(name);
    pending.gets.push(get);
    return get->promise.future();
  } else if (result.isError()) {
    return Future<Option<Entry> >::failed(result.error());
  }

  return result.get();
}


Future<bool> ZooKeeperStorageProcess::set(const Entry& entry, const UUID& uuid)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  } else if (state != CONNECTED) {
    Set* set = new Set(entry, uuid);
    pending.sets.push(set);
    return set->promise.future();
  }

  Result<bool> result = doSet(entry, uuid);

  if (result.isNone()) { // Try again later.
    Set* set = new Set(entry, uuid);
    pending.sets.push(set);
    return set->promise.future();
  } else if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  return result.get();
}


Future<bool> ZooKeeperStorageProcess::expunge(const Entry& entry)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  } else if (state != CONNECTED) {
    Expunge* expunge = new Expunge(entry);
    pending.expunges.push(expunge);
    return expunge->promise.future();
  }

  Result<bool> result = doExpunge(entry);

  if (result.isNone()) { // Try again later.
    Expunge* expunge = new Expunge(entry);
    pending.expunges.push(expunge);
    return expunge->promise.future();
  } else if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  return result.get();
}


void ZooKeeperStorageProcess::connected(bool reconnect)
{
  if (!reconnect) {
    // Authenticate if necessary (and we are connected for the first
    // time, or after a session expiration).
    if (auth.isSome()) {
      LOG(INFO) << "Authenticating with ZooKeeper using " << auth.get().scheme;

      int code = zk->authenticate(auth.get().scheme, auth.get().credentials);

      if (code != ZOK) { // TODO(benh): Authentication retries?
        error = "Failed to authenticate with ZooKeeper: " + zk->message(code);
        return;
      }
    }
  }

  state = CONNECTED;

  while (!pending.names.empty()) {
    Names* names = pending.names.front();
    Result<vector<string> > result = doNames();
    if (result.isNone()) {
      return; // Try again later.
    } else if (result.isError()) {
      names->promise.fail(result.error());
    } else {
      names->promise.set(result.get());
    }
    pending.names.pop();
    delete names;
  }

  while (!pending.gets.empty()) {
    Get* get = pending.gets.front();
    Result<Option<Entry> > result = doGet(get->name);
    if (result.isNone()) {
      return; // Try again later.
    } else if (result.isError()) {
      get->promise.fail(result.error());
    } else {
      get->promise.set(result.get());
    }
    pending.gets.pop();
    delete get;
  }

  while (!pending.sets.empty()) {
    Set* set = pending.sets.front();
    Result<bool> result = doSet(set->entry, set->uuid);
    if (result.isNone()) {
      return; // Try again later.
    } else if (result.isError()) {
      set->promise.fail(result.error());
    } else {
      set->promise.set(result.get());
    }
    pending.sets.pop();
    delete set;
  }
}


void ZooKeeperStorageProcess::reconnecting()
{
  state = CONNECTING;
}


void ZooKeeperStorageProcess::expired()
{
  state = DISCONNECTED;

  delete zk;
  zk = new ZooKeeper(servers, timeout, watcher);

  state = CONNECTING;
}


void ZooKeeperStorageProcess::updated(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStorageProcess::created(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStorageProcess::deleted(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


Result<vector<string> > ZooKeeperStorageProcess::doNames()
{
  // Get all children to determine current memberships.
  vector<string> results;

  int code = zk->getChildren(znode, false, &results);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to get children of '" + znode +
        "' in ZooKeeper: " + zk->message(code));
  }

  // TODO(benh): It might make sense to "mangle" the names so that we
  // can determine when a znode has incorrectly been added that
  // actually doesn't store an Entry.
  return results;
}


Result<Option<Entry> > ZooKeeperStorageProcess::doGet(const string& name)
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
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to get '" + znode + "/" + name +
        "' in ZooKeeper: " + zk->message(code));
  }

  google::protobuf::io::ArrayInputStream stream(result.data(), result.size());

  Entry entry;

  if (!entry.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize Entry");
  }

  return Option<Entry>::some(entry);
}


Result<bool> ZooKeeperStorageProcess::doSet(const Entry& entry, const UUID& uuid)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  // Serialize to make sure we're under the 1 MB limit.
  string data;

  if (!entry.SerializeToString(&data)) {
    return Error("Failed to serialize Entry");
  }

  if (data.size() > 1024 * 1024) { // 1 MB
    // TODO(benh): Use stout/gzip.hpp for compression.
    return Error("Serialized data is too big (> 1 MB)");
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
        return None(); // Try again later.
      } else if (code != ZOK && code != ZNODEEXISTS) {
        return Error(
            "Failed to create '" + prefix +
            "' in ZooKeeper: " + zk->message(code));
      }
    }

    code = zk->create(znode + "/" + entry.name(), data, acl, 0, NULL);

    if (code == ZNODEEXISTS) {
      return false; // Lost a race with someone else.
    } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
      CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
      return None(); // Try again later.
    } else if (code != ZOK) {
      return Error(
          "Failed to create '" + znode + "/" + entry.name() +
          "' in ZooKeeper: " + zk->message(code));
    }

    return true;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to get '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  google::protobuf::io::ArrayInputStream stream(result.data(), result.size());

  Entry current;

  if (!current.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize Entry");
  }

  if (UUID::fromBytes(current.uuid()) != uuid) {
    return false;
  }

  // Okay, do the set, we get atomicity by requiring 'stat.version'.
  code = zk->set(znode + "/" + entry.name(), data, stat.version);

  if (code == ZBADVERSION) {
    return false;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to set '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  return true;
}


Result<bool> ZooKeeperStorageProcess::doExpunge(const Entry& entry)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  string result;
  Stat stat;

  int code = zk->get(znode + "/" + entry.name(), false, &result, &stat);

  if (code == ZNONODE) {
    return false;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to get '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  google::protobuf::io::ArrayInputStream stream(result.data(), result.size());

  Entry current;

  if (!current.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize Entry");
  }

  if (UUID::fromBytes(current.uuid()) != UUID::fromBytes(entry.uuid())) {
    return false;
  }

  // Okay, do the remove, we get atomicity by requiring 'stat.version'.
  code = zk->remove(znode + "/" + entry.name(), stat.version);

  if (code == ZBADVERSION) {
    return false;
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to remove '" + znode + "/" + entry.name() +
        "' in ZooKeeper: " + zk->message(code));
  }

  return true;
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
