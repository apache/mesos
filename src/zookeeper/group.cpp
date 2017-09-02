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

#include <algorithm>
#include <queue>
#include <utility>
#include <vector>

#include <mesos/zookeeper/group.hpp>
#include <mesos/zookeeper/watcher.hpp>
#include <mesos/zookeeper/zookeeper.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include <stout/os/constants.hpp>

#include "logging/logging.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::queue;
using std::set;
using std::string;
using std::vector;


namespace zookeeper {

// Time to wait after retryable errors.
const Duration GroupProcess::RETRY_INTERVAL = Seconds(2);


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


// Helper for discarding a queue of promises.
template <typename T>
void discard(queue<T*>* queue)
{
  while (!queue->empty()) {
    T* t = queue->front();
    queue->pop();
    t->promise.discard();
    delete t;
  }
}


GroupProcess::GroupProcess(
    const string& _servers,
    const Duration& _sessionTimeout,
    const string& _znode,
    const Option<Authentication>& _auth)
  : ProcessBase(ID::generate("zookeeper-group")),
    servers(_servers),
    sessionTimeout(_sessionTimeout),
    znode(strings::remove(_znode, "/", strings::SUFFIX)),
    auth(_auth),
    acl(_auth.isSome()
        ? EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    watcher(nullptr),
    zk(nullptr),
    state(DISCONNECTED),
    retrying(false)
{}


GroupProcess::GroupProcess(
    const URL& url,
    const Duration& sessionTimeout)
  : GroupProcess(
        url.servers,
        sessionTimeout,
        strings::remove(url.path, "/", strings::SUFFIX),
        url.authentication)
{}


// NB: The `retry` and `connect` timers might still be active. However,
// we don't need to clean them up -- when the timers fire, they will
// attempt to dispatch to a no-longer-valid PID, which is a no-op.
GroupProcess::~GroupProcess()
{
  discard(&pending.joins);
  discard(&pending.cancels);
  discard(&pending.datas);
  discard(&pending.watches);

  delete zk;
  delete watcher;
}


void GroupProcess::initialize()
{
  // Doing initialization here allows to avoid the race between
  // instantiating the ZooKeeper instance and being spawned ourself.
  startConnection();
}


void GroupProcess::startConnection()
{
  watcher = new ProcessWatcher<GroupProcess>(self());
  zk = new ZooKeeper(servers, sessionTimeout, watcher);
  state = CONNECTING;

  // If the connection is not established within the session timeout,
  // close the ZooKeeper handle and create a new one. This is
  // important because the ZooKeeper 3.4 client libraries don't try to
  // re-resolve the list of hostnames, so we create a new ZooKeeper
  // handle to ensure we observe DNS changes. See MESOS-4546 and
  // `ZooKeeperProcess::initialize` for more information.
  CHECK_NONE(connectTimer);
  connectTimer = delay(zk->getSessionTimeout(),
                       self(),
                       &Self::timedout,
                       zk->getSessionId());
}


Future<Group::Membership> GroupProcess::join(
    const string& data,
    const Option<string>& label)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != READY) {
    Join* join = new Join(data, label);
    pending.joins.push(join);
    return join->promise.future();
  }

  // TODO(benh): Write a test to see how ZooKeeper fails setting znode
  // data when the data is larger than 1 MB so we know whether or not
  // to check for that here.

  // TODO(benh): Only attempt if the pending queue is empty so that a
  // client can assume a happens-before ordering of operations (i.e.,
  // the first request will happen before the second, etc).

  Result<Group::Membership> membership = doJoin(data, label);

  if (membership.isNone()) { // Try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
    Join* join = new Join(data, label);
    pending.joins.push(join);
    return join->promise.future();
  } else if (membership.isError()) {
    return Failure(membership.error());
  }

  return membership.get();
}


Future<bool> GroupProcess::cancel(const Group::Membership& membership)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (owned.count(membership.id()) == 0) {
    // TODO(benh): Should this be an error? Right now a user can't
    // differentiate when 'false' means they can't cancel because it's
    // not owned or because it's already been cancelled (explicitly by
    // them or implicitly due to session expiration or operator
    // error).
    return false;
  }

  if (state != READY) {
    Cancel* cancel = new Cancel(membership);
    pending.cancels.push(cancel);
    return cancel->promise.future();
  }

  // TODO(benh): Only attempt if the pending queue is empty so that a
  // client can assume a happens-before ordering of operations (i.e.,
  // the first request will happen before the second, etc).

  Result<bool> cancellation = doCancel(membership);

  if (cancellation.isNone()) { // Try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
    Cancel* cancel = new Cancel(membership);
    pending.cancels.push(cancel);
    return cancel->promise.future();
  } else if (cancellation.isError()) {
    return Failure(cancellation.error());
  }

  return cancellation.get();
}


Future<Option<string>> GroupProcess::data(const Group::Membership& membership)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != READY) {
    Data* data = new Data(membership);
    pending.datas.push(data);
    return data->promise.future();
  }

  // TODO(benh): Only attempt if the pending queue is empty so that a
  // client can assume a happens-before ordering of operations (i.e.,
  // the first request will happen before the second, etc).

  Result<Option<string>> result = doData(membership);

  if (result.isNone()) { // Try again later.
    Data* data = new Data(membership);
    pending.datas.push(data);
    return data->promise.future();
  } else if (result.isError()) {
    return Failure(result.error());
  }

  return result.get();
}


Future<set<Group::Membership>> GroupProcess::watch(
    const set<Group::Membership>& expected)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != READY) {
    Watch* watch = new Watch(expected);
    pending.watches.push(watch);
    return watch->promise.future();
  }

  // To guarantee causality, we must invalidate our cache of
  // memberships after any updates are made to the group (i.e., joins
  // and cancels). This is because a client that just learned of a
  // successful join shouldn't invoke watch and get a set of
  // memberships without their membership present (which is possible
  // if we return a cache of memberships that hasn't yet been updated
  // via a ZooKeeper event) unless that membership has since expired
  // (or been deleted, e.g., via operator error). Thus, we do a
  // membership "roll call" for each watch in order to make sure all
  // causal relationships are satisfied.

  if (memberships.isNone()) {
    Try<bool> cached = cache();

    if (cached.isError()) {
      // Non-retryable error.
      return Failure(cached.error());
    } else if (!cached.get()) {
      CHECK_NONE(memberships);

      // Try again later.
      if (!retrying) {
        delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
        retrying = true;
      }
      Watch* watch = new Watch(expected);
      pending.watches.push(watch);
      return watch->promise.future();
    }
  }

  CHECK_SOME(memberships);

  if (memberships.get() == expected) { // Just wait for updates.
    Watch* watch = new Watch(expected);
    pending.watches.push(watch);
    return watch->promise.future();
  }

  return memberships.get();
}


Future<Option<int64_t>> GroupProcess::session()
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state == CONNECTING) {
    return None();
  }

  return Some(zk->getSessionId());
}


void GroupProcess::connected(int64_t sessionId, bool reconnect)
{
  if (error.isSome() || sessionId != zk->getSessionId()) {
    return;
  }

  LOG(INFO) << "Group process (" << self() << ") "
            << (reconnect ? "reconnected" : "connected") << " to ZooKeeper";

  if (!reconnect) {
    // This is the first time the ZooKeeper client connects to
    // ZooKeeper service. (It could be also the first time for the
    // group or after session expiration which causes a new ZooKeeper
    // client instance to be created.)
    CHECK_EQ(state, CONNECTING);
    state = CONNECTED;
  } else {
    // This means we are reconnecting within the same ZooKeeper
    // session. We could have completed authenticate() or create()
    // before we lost the connection (thus the state can be the any
    // of the following three) so 'sync()' below will check the state
    // and only execute necessary operations accordingly.
    CHECK(state == CONNECTED || state == AUTHENTICATED || state == READY)
      << state;
  }

  // Cancel and cleanup the connect timer. The timer should always be
  // set, because it is set before making the initial connection
  // attempt and whenever a reconnection attempt is made.
  CHECK_SOME(connectTimer);

  // Now that we are connected, we'll learn about a subsequent
  // disconnection event via the `reconnecting` callback. At that
  // point we'll also restart the `connectTimer` to ensure we retry
  // the reconnection attempt.
  Clock::cancel(connectTimer.get());
  connectTimer = None();

  // Sync group operations (and set up the group on ZK).
  Try<bool> synced = sync();

  if (synced.isError()) {
    // Non-retryable error. Abort.
    abort(synced.error());
  } else if (!synced.get()) {
    // Retryable error.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
  }
}


Try<bool> GroupProcess::authenticate()
{
  CHECK_EQ(state, CONNECTED);

  // Authenticate if necessary.
  if (auth.isSome()) {
    LOG(INFO) << "Authenticating with ZooKeeper using " << auth->scheme;

    int code = zk->authenticate(auth->scheme, auth->credentials);

    if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
      return false;
    } else if (code != ZOK) {
      return Error(
          "Failed to authenticate with ZooKeeper: " + zk->message(code));
    }
  }

  state = AUTHENTICATED;
  return true;
}


Try<bool> GroupProcess::create()
{
  CHECK_EQ(state, AUTHENTICATED);

  // Create znode path (including intermediate znodes) as necessary.
  CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');

  LOG(INFO) << "Trying to create path '" << znode << "' in ZooKeeper";

  int code = zk->create(znode, "", acl, 0, nullptr, true);

  // We fail all non-retryable return codes except ZNONODEEXISTS (
  // since that means the path we were trying to create exists). Note
  // that it's also possible we got back a ZNONODE because we could
  // not create one of the intermediate znodes (in which case we'll
  // abort in the 'else if' below since ZNONODE is non-retryable).
  // Also note that it's possible that the intermediate path exists
  // but we don't have permission to know it, in this case we abort
  // as well to be on the safe side
  // TODO(benh): Need to check that we also can put a watch on the
  // children of 'znode'.
  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK_NE(zk->getState(), ZOO_AUTH_FAILED_STATE);
    return false;
  } else if (code != ZOK && code != ZNODEEXISTS) {
    return Error(
        "Failed to create '" + znode + "' in ZooKeeper: " + zk->message(code));
  }

  state = READY;
  return true;
}


void GroupProcess::reconnecting(int64_t sessionId)
{
  if (error.isSome() || sessionId != zk->getSessionId()) {
    return;
  }

  LOG(INFO) << "Lost connection to ZooKeeper, attempting to reconnect ...";

  // Set 'retrying' to false to prevent retry() from executing sync()
  // before the group reconnects to ZooKeeper. The group will sync
  // with ZooKeeper after it is connected.
  retrying = false;

  // ZooKeeper won't tell us of a session expiration until we
  // reconnect, which could occur much much later than the session was
  // actually expired. This can lead to a prolonged split-brain
  // scenario when network partitions occur. Rather than wait for a
  // reconnection to occur (i.e., a network partition to be repaired)
  // we create a local timer and "expire" our session prematurely if
  // we haven't reconnected within the session expiration time out.
  // The timer can be reset if the connection is restored.

  // We expect to see exactly one `reconnecting` event when our
  // session is disconnected, even if we're disconnected for an
  // extended period. Since we clear the `connectTimer` when the
  // connection is established, it should still be unset here.
  CHECK_NONE(connectTimer);

  // Use the negotiated session timeout for the connect timer.
  connectTimer = delay(zk->getSessionTimeout(),
                       self(),
                       &Self::timedout,
                       zk->getSessionId());
}


void GroupProcess::timedout(int64_t sessionId)
{
  if (error.isSome()) {
    return;
  }

  CHECK_NOTNULL(zk);

  // The connect timer can be reset or replaced and `zk`
  // can be replaced since this method was dispatched.
  if (connectTimer.isSome() &&
      connectTimer->timeout().expired() &&
      zk->getSessionId() == sessionId) {
    LOG(WARNING) << "Timed out waiting to connect to ZooKeeper. "
                 << "Forcing ZooKeeper session "
                 << "(sessionId=" << std::hex << sessionId << ") expiration";

    // Locally determine that the current session has expired.
    dispatch(self(), &Self::expired, zk->getSessionId());
  }
}


void GroupProcess::expired(int64_t sessionId)
{
  if (error.isSome() || sessionId != zk->getSessionId()) {
    return;
  }

  LOG(INFO) << "ZooKeeper session expired";

  // Cancel the retries. Group will sync() after it reconnects to ZK.
  retrying = false;

  // Cancel and cleanup the connect timer (if necessary).
  if (connectTimer.isSome()) {
    Clock::cancel(connectTimer.get());
    connectTimer = None();
  }

  // From the group's local perspective all the memberships are
  // gone so we need to update the watches.
  // If the memberships still exist on ZooKeeper, they will be
  // restored in group after the group reconnects to ZK.
  // This is a precaution against the possibility that ZK connection
  // is lost right after we recreate the ZK instance below or the
  // entire ZK cluster goes down. The outage can last for a long time
  // but the clients watching the group should be informed sooner.
  memberships = set<Group::Membership>();
  update();

  // Invalidate the cache so that we'll sync with ZK after
  // reconnection.
  memberships = None();

  // Set all owned memberships as cancelled.
  foreachpair (int32_t sequence, Promise<bool>* cancelled, utils::copy(owned)) {
    cancelled->set(false); // Since this was not requested.
    owned.erase(sequence); // Okay since iterating over a copy.
    delete cancelled;
  }

  CHECK(owned.empty());

  // Note that we DO NOT clear unowned. The next time we try and cache
  // the memberships we'll trigger any cancelled unowned memberships
  // then. We could imagine doing this for owned memberships too, but
  // for now we proactively cancel them above.

  state = DISCONNECTED;

  delete CHECK_NOTNULL(zk);
  delete CHECK_NOTNULL(watcher);
  startConnection();
}


void GroupProcess::updated(int64_t sessionId, const string& path)
{
  if (error.isSome() || sessionId != zk->getSessionId()) {
    return;
  }

  CHECK_EQ(znode, path);

  Try<bool> cached = cache(); // Update cache (will invalidate first).

  if (cached.isError()) {
    abort(cached.error()); // Cancel everything pending.
  } else if (!cached.get()) {
    CHECK_NONE(memberships);

    // Try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
  } else {
    update(); // Update any pending watches.
  }
}


void GroupProcess::created(int64_t sessionId, const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void GroupProcess::deleted(int64_t sessionId, const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


Result<Group::Membership> GroupProcess::doJoin(
    const string& data,
    const Option<string>& label)
{
  CHECK_EQ(state, READY);

  const string path = znode + "/" + (label.isSome() ? (label.get() + "_") : "");

  // Create a new ephemeral node to represent a new member and use the
  // the specified data as its contents.
  string result;

  const int code = zk->create(
      path,
      data,
      acl,
      ZOO_SEQUENCE | ZOO_EPHEMERAL,
      &result);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK_NE(zk->getState(), ZOO_AUTH_FAILED_STATE);
    return None();
  } else if (code != ZOK) {
    return Error(
        "Failed to create ephemeral node at '" + path +
        "' in ZooKeeper: " + zk->message(code));
  }

  // Invalidate the cache (it will/should get immediately populated
  // via the 'updated' callback of our ZooKeeper watcher).
  memberships = None();

  // Save the sequence number but only grab the basename. Example:
  // "/path/to/znode/label_0000000131" => "0000000131".
  const string basename = strings::tokenize(result, "/").back();

  // Strip the label before grabbing the sequence number.
  const string node = label.isSome()
      ? strings::remove(basename, label.get() + "_")
      : basename;

  Try<int32_t> sequence = numify<int32_t>(node);
  CHECK_SOME(sequence);

  Promise<bool>* cancelled = new Promise<bool>();
  owned[sequence.get()] = cancelled;

  return Group::Membership(sequence.get(), label, cancelled->future());
}


Result<bool> GroupProcess::doCancel(const Group::Membership& membership)
{
  CHECK_EQ(state, READY);

  string path = path::join(
      znode,
      zkBasename(membership),
      os::POSIX_PATH_SEPARATOR);

  LOG(INFO) << "Trying to remove '" << path << "' in ZooKeeper";

  // Remove ephemeral node.
  int code = zk->remove(path, -1);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK_NE(zk->getState(), ZOO_AUTH_FAILED_STATE);
    return None();
  } else if (code == ZNONODE) {
    // This can happen because the membership could have expired but
    // we have yet to receive the update about it.
    return false;
  } else if (code != ZOK) {
    return Error(
        "Failed to remove ephemeral node '" + path +
        "' in ZooKeeper: " + zk->message(code));
  }

  // Invalidate the cache (it will/should get immediately populated
  // via the 'updated' callback of our ZooKeeper watcher).
  memberships = None();

  // Let anyone waiting know the membership has been cancelled.
  CHECK(owned.count(membership.id()) == 1);
  Promise<bool>* cancelled = owned[membership.id()];
  cancelled->set(true);
  owned.erase(membership.id());
  delete cancelled;

  return true;
}


Result<Option<string>> GroupProcess::doData(
    const Group::Membership& membership)
{
  CHECK_EQ(state, READY);

  string path = path::join(
      znode,
      zkBasename(membership),
      os::POSIX_PATH_SEPARATOR);

  LOG(INFO) << "Trying to get '" << path << "' in ZooKeeper";

  // Get data associated with ephemeral node.
  string result;

  int code = zk->get(path, false, &result, nullptr);

  if (code == ZNONODE) {
    return Option<string>::none();
  } else if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK_NE(zk->getState(), ZOO_AUTH_FAILED_STATE);
    return None(); // Try again later.
  } else if (code != ZOK) {
    return Error(
        "Failed to get data for ephemeral node '" + path +
        "' in ZooKeeper: " + zk->message(code));
  }

  return Some(result);
}


Try<bool> GroupProcess::cache()
{
  // Invalidate first (if it's not already).
  memberships = None();

  // Get all children to determine current memberships.
  vector<string> results;

  int code = zk->getChildren(znode, true, &results); // Sets the watch!

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK_NE(zk->getState(), ZOO_AUTH_FAILED_STATE);
    return false;
  } else if (code != ZOK) {
    return Error("Non-retryable error attempting to get children of '" + znode
                 + "' in ZooKeeper: " + zk->message(code));
  }

  // Convert results to sequence numbers and (optionally) labels.
  hashmap<int32_t, Option<string>> sequences;

  foreach (const string& result, results) {
    vector<string> tokens = strings::tokenize(result, "_");
    Option<string> label = None();
    if (tokens.size() > 1) {
      label = tokens[0];
    }

    Try<int32_t> sequence = numify<int32_t>(tokens.back());

    // Skip it if it couldn't be converted to a number.
    // NOTE: This is currently possible when using a replicated log
    // based registry because the log replicas register under
    // "/log_replicas" at the same path as the masters' ephemeral
    // znodes.
    if (sequence.isError()) {
      VLOG(1) << "Found non-sequence node '" << result
              << "' at '" << znode << "' in ZooKeeper";
      continue;
    }

    sequences[sequence.get()] = label;
  }

  // Cache current memberships, cancelling those that are now missing.
  set<Group::Membership> current;

  foreachpair (int32_t sequence, Promise<bool>* cancelled, utils::copy(owned)) {
    if (!sequences.contains(sequence)) {
      cancelled->set(false);
      owned.erase(sequence); // Okay since iterating over a copy.
      delete cancelled;
    } else {
      current.insert(Group::Membership(
          sequence, sequences[sequence], cancelled->future()));

      sequences.erase(sequence);
    }
  }

  foreachpair (int32_t sequence,
               Promise<bool>* cancelled,
               utils::copy(unowned)) {
    if (!sequences.contains(sequence)) {
      cancelled->set(false);
      unowned.erase(sequence); // Okay since iterating over a copy.
      delete cancelled;
    } else {
      current.insert(Group::Membership(
          sequence, sequences[sequence], cancelled->future()));

      sequences.erase(sequence);
    }
  }

  // Add any remaining (i.e., unexpected) sequences.
  foreachpair (int32_t sequence, const Option<string>& label, sequences) {
    Promise<bool>* cancelled = new Promise<bool>();
    unowned[sequence] = cancelled;
    current.insert(Group::Membership(sequence, label, cancelled->future()));
  }

  memberships = current;

  return true;
}


void GroupProcess::update()
{
  CHECK_SOME(memberships);
  const size_t size = pending.watches.size();
  for (size_t i = 0; i < size; i++) {
    Watch* watch = pending.watches.front();
    if (memberships.get() != watch->expected) {
      watch->promise.set(memberships.get());
      pending.watches.pop();
      delete watch;
    } else {
      // Don't delete the watch, but push it to the back of the queue.
      pending.watches.push(watch);
      pending.watches.pop();
    }
  }
}


Try<bool> GroupProcess::sync()
{
  LOG(INFO)
    << "Syncing group operations: queue size (joins, cancels, datas) = ("
    << pending.joins.size() << ", " << pending.cancels.size() << ", "
    << pending.datas.size() << ")";

  // The state may be CONNECTED or AUTHENTICATED if Group setup has
  // not finished.
  CHECK(state == CONNECTED || state == AUTHENTICATED || state == READY)
    << state;

  // Authenticate with ZK if not already authenticated.
  if (state == CONNECTED) {
    Try<bool> authenticated = authenticate();
    if (authenticated.isError() || !authenticated.get()) {
      return authenticated;
    }
  }

  // Create group base path if not already created.
  if (state == AUTHENTICATED) {
    Try<bool> created = create();
    if (created.isError() || !created.get()) {
      return created;
    }
  }

  // Do joins.
  while (!pending.joins.empty()) {
    Join* join = pending.joins.front();
    Result<Group::Membership> membership = doJoin(join->data, join->label);
    if (membership.isNone()) {
      return false; // Try again later.
    } else if (membership.isError()) {
      join->promise.fail(membership.error());
    } else {
      join->promise.set(membership.get());
    }
    pending.joins.pop();
    delete join;
  }

  // Do cancels.
  while (!pending.cancels.empty()) {
    Cancel* cancel = pending.cancels.front();
    Result<bool> cancellation = doCancel(cancel->membership);
    if (cancellation.isNone()) {
      return false; // Try again later.
    } else if (cancellation.isError()) {
      cancel->promise.fail(cancellation.error());
    } else {
      cancel->promise.set(cancellation.get());
    }
    pending.cancels.pop();
    delete cancel;
  }

  // Do datas.
  while (!pending.datas.empty()) {
    Data* data = pending.datas.front();
    // TODO(benh): Ignore if future has been discarded?
    Result<Option<string>> result = doData(data->membership);
    if (result.isNone()) {
      return false; // Try again later.
    } else if (result.isError()) {
      data->promise.fail(result.error());
    } else {
      data->promise.set(result.get());
    }
    pending.datas.pop();
    delete data;
  }

  // Get cache of memberships if we don't have one. Note that we do
  // this last because any joins or cancels above will invalidate our
  // cache, so it would be nice to get it validated again at the
  // end. The side-effect here is that users will learn of joins and
  // cancels first through any explicit futures for them rather than
  // watches.
  if (memberships.isNone()) {
    Try<bool> cached = cache();
    if (cached.isError() || !cached.get()) {
      CHECK_NONE(memberships);
      return cached;
    } else {
      update(); // Update any pending watches.
    }
  }

  return true;
}


void GroupProcess::retry(const Duration& duration)
{
  if (!retrying) {
    // Retry could be cancelled before it is scheduled.
    return;
  }

  // We cancel the retries when the group aborts and when its ZK
  // session expires so 'retrying' should be false in the condition
  // check above.
  CHECK_NONE(error);

  // In order to be retrying, we should be at least CONNECTED.
  CHECK(state == CONNECTED || state == AUTHENTICATED || state == READY)
    << state;

  // Will reset it to true if another retry is necessary.
  retrying = false;

  Try<bool> synced = sync();

  if (synced.isError()) {
    // Non-retryable error. Abort.
    abort(synced.error());
  } else if (!synced.get()) {
    // Backoff and keep retrying.
    retrying = true;
    Seconds seconds = std::min(duration * 2, Duration(Seconds(60)));
    delay(seconds, self(), &GroupProcess::retry, seconds);
  }
}


void GroupProcess::abort(const string& message)
{
  // Set the error variable so that the group becomes unfunctional.
  error = Error(message);

  LOG(ERROR) << "Group aborting: " << message;

  // Cancel the retries.
  retrying = false;

  fail(&pending.joins, message);
  fail(&pending.cancels, message);
  fail(&pending.datas, message);
  fail(&pending.watches, message);

  // Set all owned memberships as cancelled.
  foreachvalue (Promise<bool>* cancelled, owned) {
    cancelled->set(false); // Since this was not requested.
    delete cancelled;
  }

  owned.clear();

  // Since we decided to abort, we expire the session to clean up
  // ephemeral ZNodes as necessary.
  delete CHECK_NOTNULL(zk);
  delete CHECK_NOTNULL(watcher);
  zk = nullptr;
  watcher = nullptr;
}


string GroupProcess::zkBasename(const Group::Membership& membership)
{
  Try<string> sequence = strings::format("%.*d", 10, membership.sequence);
  CHECK_SOME(sequence);

  return membership.label_.isSome()
      ? (membership.label_.get() + "_" + sequence.get())
      : sequence.get();
}


Group::Group(const string& servers,
             const Duration& sessionTimeout,
             const string& znode,
             const Option<Authentication>& auth)
{
  process = new GroupProcess(servers, sessionTimeout, znode, auth);
  spawn(process);
}


Group::Group(const URL& url,
             const Duration& sessionTimeout)
{
  process = new GroupProcess(url, sessionTimeout);
  spawn(process);
}


Group::~Group()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Group::Membership> Group::join(
    const string& data,
    const Option<string>& label)
{
  return dispatch(process, &GroupProcess::join, data, label);
}


Future<bool> Group::cancel(const Group::Membership& membership)
{
  return dispatch(process, &GroupProcess::cancel, membership);
}


Future<Option<string>> Group::data(const Group::Membership& membership)
{
  return dispatch(process, &GroupProcess::data, membership);
}


Future<set<Group::Membership>> Group::watch(
    const set<Group::Membership>& expected)
{
  return dispatch(process, &GroupProcess::watch, expected);
}


Future<Option<int64_t>> Group::session()
{
  return dispatch(process, &GroupProcess::session);
}

} // namespace zookeeper {
