// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <queue>
#include <set>
#include <string>
#include <vector>

#include <mesos/zookeeper/authentication.hpp>
#include <mesos/zookeeper/watcher.hpp>
#include <mesos/zookeeper/zookeeper.hpp>

#include <mesos/state/storage.hpp>
#include <mesos/state/zookeeper.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

using namespace process;

// Note that we don't add 'using std::set' here because we need
// 'std::' to disambiguate the 'set' member.
using std::queue;
using std::string;
using std::vector;

using mesos::internal::state::Entry;

using zookeeper::Authentication;

namespace mesos {
namespace state {


class ZooKeeperStorageProcess : public Process<ZooKeeperStorageProcess>
{
public:
  ZooKeeperStorageProcess(
      const string& servers,
      const Duration& timeout,
      const string& znode,
      const Option<Authentication>& auth);
  ~ZooKeeperStorageProcess() override;

  void initialize() override;

  // Storage implementation.
  Future<Option<Entry>> get(const string& name);
  Future<bool> set(const Entry& entry, const id::UUID& uuid);
  virtual Future<bool> expunge(const Entry& entry);
  Future<std::set<string>> names();

  // ZooKeeper events.
  // Note that events from previous sessions are dropped.
  void connected(int64_t sessionId, bool reconnect);
  void reconnecting(int64_t sessionId);
  void expired(int64_t sessionId);
  void updated(int64_t sessionId, const string& path);
  void created(int64_t sessionId, const string& path);
  void deleted(int64_t sessionId, const string& path);

private:
  // Helpers for getting the names, fetching, and swapping.
  Result<std::set<string>> doNames();
  Result<Option<Entry>> doGet(const string& name);
  Result<bool> doSet(const Entry& entry, const id::UUID& uuid);
  Result<bool> doExpunge(const Entry& entry);

  const string servers;

  // The session timeout requested by the client.
  const Duration timeout;

  const string znode;

  Option<Authentication> auth; // ZooKeeper authentication.

  const ACL_vector acl; // Default ACL to use.

  Watcher* watcher;
  ZooKeeper* zk;

  // ZooKeeper connection state.
  enum State
  {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

  struct Names
  {
    Promise<std::set<string>> promise;
  };

  struct Get
  {
    explicit Get(const string& _name) : name(_name) {}

    string name;
    Promise<Option<Entry>> promise;
  };

  struct Set
  {
    Set(const Entry& _entry, const id::UUID& _uuid) : entry(_entry), uuid(_uuid)
    {}

    Entry entry;
    id::UUID uuid;
    Promise<bool> promise;
  };

  struct Expunge
  {
    explicit Expunge(const Entry& _entry) : entry(_entry) {}

    Entry entry;
    Promise<bool> promise;
  };

  // TODO(benh): Make pending a single queue of "operations" that can
  // be "invoked" (C++11 lambdas would help).
  struct {
    queue<Names*> names;
    queue<Get*> gets;
    queue<Set*> sets;
    queue<Expunge*> expunges;
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


ZooKeeperStorageProcess::ZooKeeperStorageProcess(
    const string& _servers,
    const Duration& _timeout,
    const string& _znode,
    const Option<Authentication>& _auth)
  : ProcessBase(process::ID::generate("zookeeper-storage")),
    servers(_servers),
    timeout(_timeout),
    znode(strings::remove(_znode, "/", strings::SUFFIX)),
    auth(_auth),
    acl(_auth.isSome()
        ? zookeeper::EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    watcher(nullptr),
    zk(nullptr),
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


Future<std::set<string>> ZooKeeperStorageProcess::names()
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != CONNECTED) {
    Names* names = new Names();
    pending.names.push(names);
    return names->promise.future();
  }

  Result<std::set<string>> result = doNames();

  if (result.isNone()) { // Try again later.
    Names* names = new Names();
    pending.names.push(names);
    return names->promise.future();
  } else if (result.isError()) {
    return Failure(result.error());
  }

  return result.get();
}


Future<Option<Entry>> ZooKeeperStorageProcess::get(const string& name)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != CONNECTED) {
    Get* get = new Get(name);
    pending.gets.push(get);
    return get->promise.future();
  }

  Result<Option<Entry>> result = doGet(name);

  if (result.isNone()) { // Try again later.
    Get* get = new Get(name);
    pending.gets.push(get);
    return get->promise.future();
  } else if (result.isError()) {
    return Failure(result.error());
  }

  return result.get();
}


Future<bool> ZooKeeperStorageProcess::set(
    const Entry& entry,
    const id::UUID& uuid)
{
  if (error.isSome()) {
    return Failure(error.get());
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
    return Failure(result.error());
  }

  return result.get();
}


Future<bool> ZooKeeperStorageProcess::expunge(const Entry& entry)
{
  if (error.isSome()) {
    return Failure(error.get());
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
    return Failure(result.error());
  }

  return result.get();
}


void ZooKeeperStorageProcess::connected(int64_t sessionId, bool reconnect)
{
  if (sessionId != zk->getSessionId()) {
    return;
  }

  if (!reconnect) {
    // Authenticate if necessary (and we are connected for the first
    // time, or after a session expiration).
    if (auth.isSome()) {
      LOG(INFO) << "Authenticating with ZooKeeper using " << auth->scheme;

      int code = zk->authenticate(auth->scheme, auth->credentials);

      if (code != ZOK) { // TODO(benh): Authentication retries?
        error = "Failed to authenticate with ZooKeeper: " + zk->message(code);
        return;
      }
    }
  }

  state = CONNECTED;

  while (!pending.names.empty()) {
    Names* names = pending.names.front();
    Result<std::set<string>> result = doNames();
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
    Result<Option<Entry>> result = doGet(get->name);
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


void ZooKeeperStorageProcess::reconnecting(int64_t sessionId)
{
  if (sessionId != zk->getSessionId()) {
    return;
  }

  state = CONNECTING;
}


void ZooKeeperStorageProcess::expired(int64_t sessionId)
{
  if (sessionId != zk->getSessionId()) {
    return;
  }

  state = DISCONNECTED;

  delete zk;
  zk = new ZooKeeper(servers, timeout, watcher);

  state = CONNECTING;
}


void ZooKeeperStorageProcess::updated(int64_t sessionId, const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStorageProcess::created(int64_t sessionId, const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void ZooKeeperStorageProcess::deleted(int64_t sessionId, const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


Result<std::set<string>> ZooKeeperStorageProcess::doNames()
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
  return std::set<string>(results.begin(), results.end());
}


Result<Option<Entry>> ZooKeeperStorageProcess::doGet(const string& name)
{
  CHECK_NONE(error) << ": " << error.get();
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

  return Some(entry);
}


Result<bool> ZooKeeperStorageProcess::doSet(const Entry& entry,
                                            const id::UUID& uuid)
{
  CHECK_NONE(error) << ": " << error.get();
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
    size_t index = znode.find('/', 0);

    while (index < string::npos) {
      // Get out the prefix to create.
      index = znode.find('/', index + 1);
      string prefix = znode.substr(0, index);

      // Create the znode (even if it already exists).
      code = zk->create(prefix, "", acl, 0, nullptr);

      if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
        CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
        return None(); // Try again later.
      } else if (code != ZOK && code != ZNODEEXISTS) {
        return Error(
            "Failed to create '" + prefix +
            "' in ZooKeeper: " + zk->message(code));
      }
    }

    code = zk->create(znode + "/" + entry.name(), data, acl, 0, nullptr);

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

  if (id::UUID::fromBytes(current.uuid()).get() != uuid) {
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
  CHECK_NONE(error) << ": " << error.get();
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

  if (id::UUID::fromBytes(current.uuid()).get() !=
      id::UUID::fromBytes(entry.uuid()).get()) {
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


ZooKeeperStorage::ZooKeeperStorage(
    const string& servers,
    const Duration& timeout,
    const string& znode,
    const Option<Authentication>& auth)
{
  process = new ZooKeeperStorageProcess(servers, timeout, znode, auth);
  spawn(process);
}


ZooKeeperStorage::~ZooKeeperStorage()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Option<Entry>> ZooKeeperStorage::get(const string& name)
{
  return dispatch(process, &ZooKeeperStorageProcess::get, name);
}


Future<bool> ZooKeeperStorage::set(const Entry& entry, const id::UUID& uuid)
{
  return dispatch(process, &ZooKeeperStorageProcess::set, entry, uuid);
}


Future<bool> ZooKeeperStorage::expunge(const Entry& entry)
{
  return dispatch(process, &ZooKeeperStorageProcess::expunge, entry);
}


Future<std::set<string>> ZooKeeperStorage::names()
{
  return dispatch(process, &ZooKeeperStorageProcess::names);
}

} // namespace state {
} // namespace mesos {
