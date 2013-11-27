#include <algorithm>
#include <queue>
#include <utility>
#include <vector>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include "logging/logging.hpp"

#include "zookeeper/group.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using std::make_pair;
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


GroupProcess::GroupProcess(
    const string& _servers,
    const Duration& _timeout,
    const string& _znode,
    const Option<Authentication>& _auth)
  : servers(_servers),
    timeout(_timeout),
    znode(strings::remove(_znode, "/", strings::SUFFIX)),
    auth(_auth),
    acl(_auth.isSome()
        ? EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    watcher(NULL),
    zk(NULL),
    state(DISCONNECTED),
    retrying(false)
{}


// TODO(xujyan): Reuse the peer constructor above once we switch to
// C++ 11.
GroupProcess::GroupProcess(
    const URL& url,
    const Duration& _timeout)
  : servers(url.servers),
    timeout(_timeout),
    znode(strings::remove(url.path, "/", strings::SUFFIX)),
    auth(url.authentication),
    acl(url.authentication.isSome()
        ? EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    watcher(NULL),
    zk(NULL),
    state(DISCONNECTED),
    retrying(false)
{}


GroupProcess::~GroupProcess()
{
  fail(&pending.joins, "No longer watching group");
  fail(&pending.cancels, "No longer watching group");
  fail(&pending.datas, "No longer watching group");
  fail(&pending.watches, "No longer watching group");

  delete zk;
  delete watcher;
}


void GroupProcess::initialize()
{
  // Doing initialization here allows to avoid the race between
  // instantiating the ZooKeeper instance and being spawned ourself.
  watcher = new ProcessWatcher<GroupProcess>(self());
  zk = new ZooKeeper(servers, timeout, watcher);
  state = CONNECTING;
}


Future<Group::Membership> GroupProcess::join(const string& data)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != CONNECTED) {
    Join* join = new Join(data);
    pending.joins.push(join);
    return join->promise.future();
  }

  // TODO(benh): Write a test to see how ZooKeeper fails setting znode
  // data when the data is larger than 1 MB so we know whether or not
  // to check for that here.

  // TODO(benh): Only attempt if the pending queue is empty so that a
  // client can assume a happens-before ordering of operations (i.e.,
  // the first request will happen before the second, etc).

  Result<Group::Membership> membership = doJoin(data);

  if (membership.isNone()) { // Try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
    Join* join = new Join(data);
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

  if (state != CONNECTED) {
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


Future<string> GroupProcess::data(const Group::Membership& membership)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != CONNECTED) {
    Data* data = new Data(membership);
    pending.datas.push(data);
    return data->promise.future();
  }

  // TODO(benh): Only attempt if the pending queue is empty so that a
  // client can assume a happens-before ordering of operations (i.e.,
  // the first request will happen before the second, etc).

  Result<string> result = doData(membership);

  if (result.isNone()) { // Try again later.
    Data* data = new Data(membership);
    pending.datas.push(data);
    return data->promise.future();
  } else if (result.isError()) {
    return Failure(result.error());
  }

  return result.get();
}


Future<set<Group::Membership> > GroupProcess::watch(
    const set<Group::Membership>& expected)
{
  if (error.isSome()) {
    return Failure(error.get());
  } else if (state != CONNECTED) {
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

  memberships.isSome() || cache();

  if (memberships.isNone()) { // Try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
    Watch* watch = new Watch(expected);
    pending.watches.push(watch);
    return watch->promise.future();
  } else if (memberships.get() == expected) { // Just wait for updates.
    Watch* watch = new Watch(expected);
    pending.watches.push(watch);
    return watch->promise.future();
  }

  return memberships.get();
}


Future<Option<int64_t> > GroupProcess::session()
{
  if (error.isSome()) {
    Promise<Option<int64_t> > promise;
    promise.fail(error.get());
    return promise.future();
  } else if (state != CONNECTED) {
    return None();
  }

  return Some(zk->getSessionId());
}


void GroupProcess::connected(bool reconnect)
{
  if (!reconnect) {
    // Authenticate if necessary (and we are connected for the first
    // time, or after a session expiration).
    if (auth.isSome()) {
      LOG(INFO) << "Authenticating with ZooKeeper using " << auth.get().scheme;

      int code = zk->authenticate(auth.get().scheme, auth.get().credentials);

      if (code != ZOK) { // TODO(benh): Authentication retries?
        error = "Failed to authenticate with ZooKeeper: " + zk->message(code);
        abort(); // Cancels everything pending.
        return;
      }
    }

    // Create znode path (including intermediate znodes) as necessary.
    CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');

    LOG(INFO) << "Trying to create path '" << znode << "' in ZooKeeper";

    int code = zk->create(znode, "", acl, 0, NULL, true);

    // We fail all non-OK return codes except ZNONODEEXISTS (since
    // that means the path we were trying to create exists) and
    // ZNOAUTH (since it's possible that the ACLs on 'dirname(znode)'
    // don't allow us to create a child znode but we are allowed to
    // create children of 'znode' itself, which will be determined
    // when we first do a Group::join). Note that it's also possible
    // we got back a ZNONODE because we could not create one of the
    // intermediate znodes (in which case we'll abort in the 'else'
    // below since ZNONODE is non-retryable). TODO(benh): Need to
    // check that we also can put a watch on the children of 'znode'.
    if (code == ZINVALIDSTATE ||
        (code != ZOK &&
         code != ZNODEEXISTS &&
         code != ZNOAUTH &&
         zk->retryable(code))) {
      CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
      return; // Try again later.
    } else if (code != ZOK && code != ZNODEEXISTS && code != ZNOAUTH) {
      error =
        "Failed to create '" + znode + "' in ZooKeeper: " + zk->message(code);
      abort(); // Cancels everything pending.
      return;
    }
  } else {
    LOG(INFO) << "Group process (" << self() << ")  reconnected to Zookeeper";

    // Cancel and cleanup the reconnect timer (if necessary).
    if (timer.isSome()) {
      Timer::cancel(timer.get());
      timer = None();
    }
  }

  state = CONNECTED;

  sync(); // Handle pending (and cache memberships).
}


void GroupProcess::reconnecting()
{
  LOG(INFO) << "Lost connection to ZooKeeper, attempting to reconnect ...";

  state = CONNECTING;

  // ZooKeeper won't tell us of a session expiration until we
  // reconnect, which could occur much much later than the session was
  // actually expired. This can lead to a prolonged split-brain
  // scenario when network partitions occur. Rather than wait for a
  // reconnection to occur (i.e., a network partition to be repaired)
  // we create a local timer and "expire" our session prematurely if
  // we haven't reconnected within the session expiration time out.
  // The timer can be reset if the connection is restored.
  CHECK(timer.isNone());
  timer = delay(timeout, self(), &Self::timedout, zk->getSessionId());
}


void GroupProcess::timedout(const int64_t& sessionId)
{
  CHECK_NOTNULL(zk);

  if (timer.isSome() &&
      timer.get().timeout().expired() &&
      zk->getSessionId() == sessionId) {
    // The timer can be reset or replaced and 'zk' can be replaced
    // since this method was dispatched.
    std::ostringstream error_;
    error_ << "Timed out waiting to reconnect to ZooKeeper (sessionId="
           << std::hex << sessionId << ")";
    error = error_.str();
    abort();
  }
}


void GroupProcess::expired()
{
  // Cancel and cleanup the reconnect timer (if necessary).
  if (timer.isSome()) {
    Timer::cancel(timer.get());
    timer = None();
  }

  // Invalidate the cache.
  memberships = None();

  // Set all owned memberships as cancelled.
  foreachpair (uint64_t sequence, Promise<bool>* cancelled, utils::copy(owned)) {
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
  watcher = new ProcessWatcher<GroupProcess>(self());
  zk = new ZooKeeper(servers, timeout, watcher);

  state = CONNECTING;
}


void GroupProcess::updated(const string& path)
{
  CHECK(znode == path);

  cache(); // Update cache (will invalidate first).

  if (memberships.isNone()) { // Something changed so we must try again later.
    if (!retrying) {
      delay(RETRY_INTERVAL, self(), &GroupProcess::retry, RETRY_INTERVAL);
      retrying = true;
    }
  } else {
    update(); // Update any pending watches.
  }
}


void GroupProcess::created(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


void GroupProcess::deleted(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event";
}


Result<Group::Membership> GroupProcess::doJoin(const string& data)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  // Create a new ephemeral node to represent a new member and use the
  // the specified data as it's contents.
  string result;

  int code = zk->create(znode + "/", data, acl,
                        ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None();
  } else if (code != ZOK) {
    return Error(
        "Failed to create ephemeral node at '" + znode +
        "' in ZooKeeper: " + zk->message(code));
  }

  // Invalidate the cache (it will/should get immediately populated
  // via the 'updated' callback of our ZooKeeper watcher).
  memberships = None();

  // Save the sequence number but only grab the basename. Example:
  // "/path/to/znode/0000000131" => "0000000131".
  Try<string> basename = os::basename(result);
  if (basename.isError()) {
    return Error("Failed to get the sequence number: " + basename.error());
  }

  Try<uint64_t> sequence = numify<uint64_t>(basename.get());
  CHECK_SOME(sequence);

  Promise<bool>* cancelled = new Promise<bool>();
  owned[sequence.get()] = cancelled;

  return Group::Membership(sequence.get(), cancelled->future());
}


Result<bool> GroupProcess::doCancel(const Group::Membership& membership)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  Try<string> sequence = strings::format("%.*d", 10, membership.sequence);

  CHECK_SOME(sequence);

  string path = znode + "/" + sequence.get();

  LOG(INFO) << "Trying to remove '" << path << "' in ZooKeeper";

  // Remove ephemeral node.
  int code = zk->remove(path, -1);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
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


Result<string> GroupProcess::doData(const Group::Membership& membership)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  Try<string> sequence = strings::format("%.*d", 10, membership.sequence);

  CHECK_SOME(sequence);

  string path = znode + "/" + sequence.get();

  LOG(INFO) << "Trying to get '" << path << "' in ZooKeeper";

  // Get data associated with ephemeral node.
  string result;

  int code = zk->get(path, false, &result, NULL);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return None();
  } else if (code != ZOK) {
    return Error(
        "Failed to get data for ephemeral node '" + path +
        "' in ZooKeeper: " + zk->message(code));
  }

  return result;
}


bool GroupProcess::cache()
{
  // Invalidate first (if it's not already).
  memberships = None();

  // Get all children to determine current memberships.
  vector<string> results;

  int code = zk->getChildren(znode, true, &results); // Sets the watch!

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return false;
  } else if (code != ZOK) {
    error =
      "Non-retryable error attempting to get children of '" + znode + "'"
      " in ZooKeeper: " + zk->message(code);
    abort(); // Cancels everything pending.
    return false;
  }

  // Convert results to sequence numbers.
  set<uint64_t> sequences;

  foreach (const string& result, results) {
    Try<uint64_t> sequence = numify<uint64_t>(result);

    // Skip it if it couldn't be converted to a number.
    if (sequence.isError()) {
      LOG(WARNING) << "Found non-sequence node '" << result
                   << "' at '" << znode << "' in ZooKeeper";
      continue;
    }

    sequences.insert(sequence.get());
  }

  // Cache current memberships, cancelling those that are now missing.
  set<Group::Membership> current;

  foreachpair (uint64_t sequence, Promise<bool>* cancelled, utils::copy(owned)) {
    if (sequences.count(sequence) == 0) {
      cancelled->set(false);
      owned.erase(sequence); // Okay since iterating over a copy.
      delete cancelled;
    } else {
      current.insert(Group::Membership(sequence, cancelled->future()));
      sequences.erase(sequence);
    }
  }

  foreachpair (uint64_t sequence, Promise<bool>* cancelled, utils::copy(unowned)) {
    if (sequences.count(sequence) == 0) {
      cancelled->set(false);
      unowned.erase(sequence); // Okay since iterating over a copy.
      delete cancelled;
    } else {
      current.insert(Group::Membership(sequence, cancelled->future()));
      sequences.erase(sequence);
    }
  }

  // Add any remaining (i.e., unexpected) sequences.
  foreach (uint64_t sequence, sequences) {
    Promise<bool>* cancelled = new Promise<bool>();
    unowned[sequence] = cancelled;
    current.insert(Group::Membership(sequence, cancelled->future()));
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


bool GroupProcess::sync()
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  // Do joins.
  while (!pending.joins.empty()) {
    Join* join = pending.joins.front();
    Result<Group::Membership> membership = doJoin(join->data);
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
    Result<string> result = doData(data->membership);
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
    if (!cache()) {
      return false; // Try again later (if no error).
    } else {
      update(); // Update any pending watches.
    }
  }

  return true;
}


void GroupProcess::retry(const Duration& duration)
{
  if (error.isSome() || state != CONNECTED) {
    retrying = false; // Stop retrying, we'll sync at reconnect (if no error).
  } else if (error.isNone() && state == CONNECTED) {
    bool synced = sync(); // Might get another retryable error.
    if (!synced && error.isNone()) {
      // Backoff.
      Seconds seconds = std::min(duration * 2, Duration(Seconds(60)));
      delay(seconds, self(), &GroupProcess::retry, seconds);
    } else {
      retrying = false;
    }
  }
}


void GroupProcess::abort()
{
  CHECK_SOME(error);

  fail(&pending.joins, error.get());
  fail(&pending.cancels, error.get());
  fail(&pending.datas, error.get());
  fail(&pending.watches, error.get());

  error = None();

  // If we decide to abort, make sure we expire the session
  // (cleaning up any ephemeral ZNodes as necessary). We also
  // create a new ZooKeeper instance for clients that want to
  // continue to reuse this group instance.
  expired();
}


Group::Group(const string& servers,
             const Duration& timeout,
             const string& znode,
             const Option<Authentication>& auth)
{
  process = new GroupProcess(servers, timeout, znode, auth);
  spawn(process);
}


Group::Group(const URL& url,
             const Duration& timeout)
{
  process = new GroupProcess(url, timeout);
  spawn(process);
}


Group::~Group()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Group::Membership> Group::join(const string& data)
{
  return dispatch(process, &GroupProcess::join, data);
}


Future<bool> Group::cancel(const Group::Membership& membership)
{
  return dispatch(process, &GroupProcess::cancel, membership);
}


Future<string> Group::data(const Group::Membership& membership)
{
  return dispatch(process, &GroupProcess::data, membership);
}


Future<set<Group::Membership> > Group::watch(
    const set<Group::Membership>& expected)
{
  return dispatch(process, &GroupProcess::watch, expected);
}


Future<Option<int64_t> > Group::session()
{
  return dispatch(process, &GroupProcess::session);
}

} // namespace zookeeper {
