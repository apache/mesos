#include <algorithm>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/process.hpp>

#include "common/result.hpp"
#include "common/strings.hpp"
#include "common/utils.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

using namespace process;

namespace utils = mesos::internal::utils; // TODO(benh): Pull utils out.

using process::wait; // Necessary on some OS's to disambiguate.

using std::make_pair;
using std::map;
using std::queue;
using std::set;
using std::string;
using std::vector;


namespace zookeeper {

const double RETRY_SECONDS = 2.0; // Time to wait after retryable errors.


class GroupProcess : public Process<GroupProcess>
{
public:
  GroupProcess(const string& servers,
               const seconds& timeout,
               const string& znode,
               const Option<Authentication>& auth);
  virtual ~GroupProcess();

  virtual void initialize();

  // Group implementation.
  Future<Group::Membership> join(const string& data);
  Future<bool> cancel(const Group::Membership& membership);
  Future<string> data(const Group::Membership& membership);
  Future<set<Group::Membership> > watch(
      const set<Group::Membership>& expected);
  Future<Option<int64_t> > session();

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const string& path);
  void created(const string& path);
  void deleted(const string& path);

private:
  Result<Group::Membership> doJoin(const string& data);
  Result<bool> doCancel(const Group::Membership& membership);
  Result<string> doData(const Group::Membership& membership);

  // Attempts to cache the current set of memberships.
  bool cache();

  // Updates any pending watches.
  void update();

  // Synchronizes pending operations with ZooKeeper and also attempts
  // to cache the current set of memberships if necessary.
  bool sync();

  // Generic retry method. This mechanism is "generic" in the sense
  // that it is not specific to any particular operation, but rather
  // attempts to perform all pending operations (including caching
  // memberships if necessary).
  void retry(double seconds);

  // Fails all pending operations.
  void abort();

  Option<string> error; // Potential non-retryable error.

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

  struct Join
  {
    Join(const string& _data) : data(_data) {}
    string data;
    Promise<Group::Membership> promise;
  };

  struct Cancel
  {
    Cancel(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    Promise<bool> promise;
  };

  struct Data
  {
    Data(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    Promise<string> promise;
  };

  struct Watch
  {
    Watch(const set<Group::Membership>& _expected)
      : expected(_expected) {}
    set<Group::Membership> expected;
    Promise<set<Group::Membership> > promise;
  };

  struct {
    queue<Join*> joins;
    queue<Cancel*> cancels;
    queue<Data*> datas;
    queue<Watch*> watches;
  } pending;

  bool retrying;

  // Expected ZooKeeper sequence numbers (either owned/created by this
  // group instance or not) and the promise we associate with their
  // "cancellation" (i.e., no longer part of the group).
  map<uint64_t, Promise<bool>*> owned;
  map<uint64_t, Promise<bool>*> unowned;

  // Cache of owned + unowned, where 'None' represents an invalid
  // cache and 'Some' represents a valid cache.
  Option<set<Group::Membership> > memberships;
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


GroupProcess::GroupProcess(
    const string& _servers,
    const seconds& _timeout,
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
    return Future<Group::Membership>::failed(error.get());
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
      delay(RETRY_SECONDS, self(), &GroupProcess::retry, RETRY_SECONDS);
      retrying = true;
    }
    Join* join = new Join(data);
    pending.joins.push(join);
    return join->promise.future();
  } else if (membership.isError()) {
    return Future<Group::Membership>::failed(membership.error());
  }

  return membership.get();
}


Future<bool> GroupProcess::cancel(const Group::Membership& membership)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
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
      delay(RETRY_SECONDS, self(), &GroupProcess::retry, RETRY_SECONDS);
      retrying = true;
    }
    Cancel* cancel = new Cancel(membership);
    pending.cancels.push(cancel);
    return cancel->promise.future();
  } else if (cancellation.isError()) {
    return Future<bool>::failed(cancellation.error());
  }

  return cancellation.get();
}


Future<string> GroupProcess::data(const Group::Membership& membership)
{
  if (error.isSome()) {
    return Future<string>::failed(error.get());
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
    return Future<string>::failed(result.error());
  }

  return result.get();
}


Future<set<Group::Membership> > GroupProcess::watch(
    const set<Group::Membership>& expected)
{
  if (error.isSome()) {
    return Future<set<Group::Membership> >::failed(error.get());
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
      delay(RETRY_SECONDS, self(), &GroupProcess::retry, RETRY_SECONDS);
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
    return Option<int64_t>::none();
  }

  return Option<int64_t>::some(zk->getSessionId());
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
        Try<string> message = strings::format(
            "Failed to authenticate with ZooKeeper: %s", zk->message(code));
        error = message.isSome()
          ? message.get()
          : "Failed to authenticate with ZooKeeper";
        abort(); // Cancels everything pending.
        return;
      }
    }

    // Create directory path znodes as necessary.
    CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');
    size_t index = znode.find("/", 0);

    while (index < string::npos) {
      // Get out the prefix to create.
      index = znode.find("/", index + 1);
      const string& prefix = znode.substr(0, index);

      LOG(INFO) << "Trying to create '" << prefix << "' in ZooKeeper";

      // Create the node (even if it already exists).
      int code = zk->create(prefix, "", acl, 0, NULL);

      if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
        CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
        return; // Try again later.
      } else if (code != ZOK && code != ZNODEEXISTS && code != ZNOAUTH) {
        // We fail all non-OK return codes except for ZNODEEXISTS and ZNOAUTH:
        // ZNODEEXISTS says the node in the znode path we are trying to create
        //   already exists - this is what we wanted, so we continue.
        // ZNOAUTH says we can't write the node, but it doesn't tell us
        //   whether the node already exists.  We take the optimistic approach
        //   and assume the node's parent doesn't allow us to write an already
        //   existing node.  As long as the last node in the znode path exists
        //   (or we can create it) and we can write children to that last node,
        //   we're good.  This condition is tested below when we're done trying
        //   to create the znode path.
        Try<string> message = strings::format(
            "Failed to create '%s' in ZooKeeper: %s",
            prefix, zk->message(code));
        error = message.isSome()
          ? message.get()
          : "Failed to create node in ZooKeeper";
        abort(); // Cancels everything pending.
        return;
      }
    }

    // Now check we have perms to write to the final znode - this is required
    // to run the Group.
    string result;
    int code = zk->create(znode + "/__write_test_", "", acl,
                          ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);
    if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
      CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
      return; // Try again later.
    } else if (code != ZOK) {
      Try<string> message = strings::format(
          "Unable to write to configured group path '%s' in ZooKeeper: %s",
          znode.c_str(), zk->message(code));
      error = message.isSome()
        ? message.get()
        : "Failed to create node in ZooKeeper";
      abort(); // Cancels everything pending.
      return;
    }

    // Make a best-effort only attempt to clean up our write test node -
    // it will die with our session if the attempts fails.
    zk->remove(result, -1);
  }

  state = CONNECTED;

  sync(); // Handle pending (and cache memberships).
}


void GroupProcess::reconnecting()
{
  state = CONNECTING;
}


void GroupProcess::expired()
{
  // Invalidate the cache.
  memberships = Option<set<Group::Membership> >::none();

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

  delete zk;
  zk = new ZooKeeper(servers, timeout, watcher);

  state = CONNECTING;
}


void GroupProcess::updated(const string& path)
{
  CHECK(znode == path);

  cache(); // Update cache (will invalidate first).

  if (memberships.isNone()) { // Something changed so we must try again later.
    if (!retrying) {
      delay(RETRY_SECONDS, self(), &GroupProcess::retry, RETRY_SECONDS);
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
    return Result<Group::Membership>::none();
  } else if (code != ZOK) {
    Try<string> message = strings::format(
        "Failed to create ephemeral node at '%s' in ZooKeeper: %s",
        znode, zk->message(code));
    return Result<Group::Membership>::error(
        message.isSome() ? message.get()
        : "Failed to create ephemeral node in ZooKeeper");
  }

  // Invalidate the cache (it will/should get immediately populated
  // via the 'updated' callback of our ZooKeeper watcher).
  memberships = Option<set<Group::Membership> >::none();

  // Save the sequence number but only grab the basename. Example:
  // "/path/to/znode/0000000131" => "0000000131".
  result = utils::os::basename(result);

  Try<uint64_t> sequence = utils::numify<uint64_t>(result);
  CHECK(sequence.isSome()) << sequence.error();

  Promise<bool>* cancelled = new Promise<bool>();
  owned[sequence.get()] = cancelled;

  return Group::Membership(sequence.get(), cancelled->future());
}


Result<bool> GroupProcess::doCancel(const Group::Membership& membership)
{
  CHECK(error.isNone()) << ": " << error.get();
  CHECK(state == CONNECTED);

  Try<string> sequence = strings::format("%.*d", 10, membership.sequence);

  CHECK(sequence.isSome()) << sequence.error();

  string path = znode + "/" + sequence.get();

  LOG(INFO) << "Trying to remove '" << path << "' in ZooKeeper";

  // Remove ephemeral node.
  int code = zk->remove(path, -1);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return Result<bool>::none();
  } else if (code != ZOK) {
    Try<string> message = strings::format(
        "Failed to remove ephemeral node '%s' in ZooKeeper: %s",
        path, zk->message(code));
    return Result<bool>::error(
        message.isSome() ? message.get()
        : "Failed to remove ephemeral node in ZooKeeper");
  }

  // Invalidate the cache (it will/should get immediately populated
  // via the 'updated' callback of our ZooKeeper watcher).
  memberships = Option<set<Group::Membership> >::none();

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

  CHECK(sequence.isSome()) << sequence.error();

  string path = znode + "/" + sequence.get();

  LOG(INFO) << "Trying to get '" << path << "' in ZooKeeper";

  // Get data associated with ephemeral node.
  string result;

  int code = zk->get(path, false, &result, NULL);

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return Result<string>::none();
  } else if (code != ZOK) {
    Try<string> message = strings::format(
        "Failed to get data for ephemeral node '%s' in ZooKeeper: %s",
        path, zk->message(code));
    return Result<string>::error(
        message.isSome() ? message.get()
        : "Failed to get data for ephemeral node in ZooKeeper");
  }

  return result;
}


bool GroupProcess::cache()
{
  // Invalidate first (if it's not already).
  memberships = Option<set<Group::Membership> >::none();

  // Get all children to determine current memberships.
  vector<string> results;

  int code = zk->getChildren(znode, true, &results); // Sets the watch!

  if (code == ZINVALIDSTATE || (code != ZOK && zk->retryable(code))) {
    CHECK(zk->getState() != ZOO_AUTH_FAILED_STATE);
    return false;
  } else if (code != ZOK) {
    Try<string> message = strings::format(
        "Non-retryable error attempting to get children of '%s'"
        " in ZooKeeper: %s", znode, zk->message(code));
    error = message.isSome()
      ? message.get()
      : "Non-retryable error attempting to get children in ZooKeeper";
    abort(); // Cancels everything pending.
    return false;
  }

  // Convert results to sequence numbers.
  set<uint64_t> sequences;

  foreach (const string& result, results) {
    Try<uint64_t> sequence = utils::numify<uint64_t>(result);

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
    Promise<bool>* cancelled = unowned[sequence];
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
  CHECK(memberships.isSome());
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


void GroupProcess::retry(double seconds)
{
  if (error.isSome() || state != CONNECTED) {
    retrying = false; // Stop retrying, we'll sync at reconnect (if no error).
  } else if (error.isNone() && state == CONNECTED) {
    bool synced = sync(); // Might get another retryable error.
    if (!synced && error.isNone()) {
      seconds = std::min(seconds * 2.0, 60.0); // Backoff.
      delay(seconds, self(), &GroupProcess::retry, seconds);
    } else {
      retrying = false;
    }
  }
}


void GroupProcess::abort()
{
  CHECK(error.isSome());

  fail(&pending.joins, error.get());
  fail(&pending.cancels, error.get());
  fail(&pending.datas, error.get());
  fail(&pending.watches, error.get());
}


Group::Group(const string& servers,
             const seconds& timeout,
             const string& znode,
             const Option<Authentication>& auth)
{
  process = new GroupProcess(servers, timeout, znode, auth);
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
