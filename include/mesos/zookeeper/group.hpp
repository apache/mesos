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

#ifndef __MESOS_ZOOKEEPER_GROUP_HPP__
#define __MESOS_ZOOKEEPER_GROUP_HPP__

#include <map>
#include <set>
#include <string>

#include <mesos/zookeeper/authentication.hpp>
#include <mesos/zookeeper/url.hpp>

#include <process/future.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

// Forward declarations.
class Watcher;
class ZooKeeper;

namespace zookeeper {

// Forward declaration.
class GroupProcess;

// Represents a distributed group managed by ZooKeeper. A group is
// associated with a specific ZooKeeper path, and members are
// represented by ephemeral sequential nodes.
class Group
{
public:
  // Represents a group membership. Note that we order memberships by
  // membership id (that is, an older membership is ordered before a
  // younger membership). In addition, we do not use the "cancelled"
  // future to compare memberships so that two memberships created
  // from different Group instances will still be considered the same.
  struct Membership
  {
    bool operator==(const Membership& that) const
    {
      return sequence == that.sequence;
    }

    bool operator!=(const Membership& that) const
    {
      return sequence != that.sequence;
    }

    bool operator<(const Membership& that) const
    {
      return sequence < that.sequence;
    }

    bool operator<=(const Membership& that) const
    {
      return sequence <= that.sequence;
    }

    bool operator>(const Membership& that) const
    {
      return sequence > that.sequence;
    }

    bool operator>=(const Membership& that) const
    {
      return sequence >= that.sequence;
    }

    int32_t id() const
    {
      return sequence;
    }

    Option<std::string> label() const
    {
      return label_;
    }

    // Returns a future that is only satisfied once this membership
    // has been cancelled. In which case, the value of the future is
    // true if you own this membership and cancelled it by invoking
    // Group::cancel. Otherwise, the value of the future is false (and
    // could signify cancellation due to a session expiration or
    // operator error).
    process::Future<bool> cancelled() const
    {
      return cancelled_;
    }

  private:
    friend class GroupProcess; // Creates and manages memberships.

    Membership(int32_t _sequence,
               const Option<std::string>& _label,
               const process::Future<bool>& cancelled)
      : sequence(_sequence), label_(_label), cancelled_(cancelled) {}

    const int32_t sequence;
    const Option<std::string> label_;
    process::Future<bool> cancelled_;
  };

  // Constructs this group using the specified ZooKeeper servers (list
  // of host:port) with the given session timeout at the specified znode.
  Group(const std::string& servers,
        const Duration& sessionTimeout,
        const std::string& znode,
        const Option<Authentication>& auth = None());
  Group(const URL& url,
        const Duration& sessionTimeout);

  ~Group();

  // Returns the result of trying to join a "group" in ZooKeeper.
  // If "label" is provided the newly created znode contains "label_"
  // as the prefix. If join is successful, an "owned" membership will
  // be returned whose retrievable data will be a copy of the
  // specified parameter. A membership is not "renewed" in the event
  // of a ZooKeeper session expiration. Instead, a client should watch
  // the group memberships and rejoin the group as appropriate.
  process::Future<Membership> join(
      const std::string& data,
      const Option<std::string>& label = None());

  // Returns the result of trying to cancel a membership. Note that
  // only memberships that are "owned" (see join) can be canceled.
  process::Future<bool> cancel(const Membership& membership);

  // Returns the result of trying to fetch the data associated with a
  // group membership.
  // A None is returned if the specified membership doesn't exist,
  // e.g., it can be removed before this call can read it content.
  process::Future<Option<std::string>> data(const Membership& membership);

  // Returns a future that gets set when the group memberships differ
  // from the "expected" memberships specified.
  process::Future<std::set<Membership>> watch(
      const std::set<Membership>& expected = std::set<Membership>());

  // Returns the current ZooKeeper session associated with this group,
  // or none if no session currently exists.
  process::Future<Option<int64_t>> session();

  // Made public for testing purposes.
  GroupProcess* process;
};


class GroupProcess : public process::Process<GroupProcess>
{
public:
  GroupProcess(const std::string& servers,
               const Duration& sessionTimeout,
               const std::string& znode,
               const Option<Authentication>& auth);

  GroupProcess(const URL& url,
               const Duration& sessionTimeout);

  ~GroupProcess() override;

  void initialize() override;

  static const Duration RETRY_INTERVAL;

  // Helper function that returns the basename of the znode of
  // the membership.
  static std::string zkBasename(const Group::Membership& membership);

  // Group implementation.
  process::Future<Group::Membership> join(
      const std::string& data,
      const Option<std::string>& label);
  process::Future<bool> cancel(const Group::Membership& membership);
  process::Future<Option<std::string>> data(
      const Group::Membership& membership);
  process::Future<std::set<Group::Membership>> watch(
      const std::set<Group::Membership>& expected);
  process::Future<Option<int64_t>> session();

  // ZooKeeper events.
  // Note that events from previous sessions are dropped.
  void connected(int64_t sessionId, bool reconnect);
  void reconnecting(int64_t sessionId);
  void expired(int64_t sessionId);
  void updated(int64_t sessionId, const std::string& path);
  void created(int64_t sessionId, const std::string& path);
  void deleted(int64_t sessionId, const std::string& path);

private:
  void startConnection();

  Result<Group::Membership> doJoin(
      const std::string& data,
      const Option<std::string>& label);
  Result<bool> doCancel(const Group::Membership& membership);
  Result<Option<std::string>> doData(const Group::Membership& membership);

  // Returns true if authentication is successful, false if the
  // failure is retryable and Error otherwise.
  Try<bool> authenticate();

  // Creates the group (which means creating its base path) on ZK.
  // Returns true if successful, false if the failure is retryable
  // and Error otherwise.
  Try<bool> create();

  // Attempts to cache the current set of memberships.
  // Returns true if successful, false if the failure is retryable
  // and Error otherwise.
  Try<bool> cache();

  // Synchronizes pending operations with ZooKeeper and also attempts
  // to cache the current set of memberships if necessary.
  // Returns true if successful, false if the failure is retryable
  // and Error otherwise.
  Try<bool> sync();

  // Updates any pending watches.
  void update();

  // Generic retry method. This mechanism is "generic" in the sense
  // that it is not specific to any particular operation, but rather
  // attempts to perform all pending operations (including caching
  // memberships if necessary).
  void retry(const Duration& duration);

  void timedout(int64_t sessionId);

  // Aborts the group instance and fails all pending operations.
  // The group then enters an error state and all subsequent
  // operations will fail as well.
  void abort(const std::string& message);

  // Potential non-retryable error set by abort().
  Option<Error> error;

  const std::string servers;

  // The session timeout requested by the client.
  const Duration sessionTimeout;

  const std::string znode;

  Option<Authentication> auth; // ZooKeeper authentication.

  const ACL_vector acl; // Default ACL to use.

  Watcher* watcher;
  ZooKeeper* zk;

  // Group connection state.
  // Normal state transitions:
  //   DISCONNECTED -> CONNECTING -> CONNECTED -> AUTHENTICATED
  //   -> READY.
  // Reconnection does not change the current state and the state is
  // only reset to DISCONNECTED after session expiration. Therefore
  // the client's "progress" in setting up the group is preserved
  // across reconnections. This means authenticate() and create() are
  // only successfully executed once in one ZooKeeper session.
  enum State
  {
    DISCONNECTED,  // The initial state.
    CONNECTING,    // ZooKeeper connecting.
    CONNECTED,     // ZooKeeper connected but before group setup.
    AUTHENTICATED, // ZooKeeper connected and authenticated.
    READY,         // ZooKeeper connected, session authenticated and
                   // base path for the group created.
  } state;

  struct Join
  {
    Join(const std::string& _data, const Option<std::string>& _label)
      : data(_data), label(_label) {}
    std::string data;
    const Option<std::string> label;
    process::Promise<Group::Membership> promise;
  };

  struct Cancel
  {
    explicit Cancel(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    process::Promise<bool> promise;
  };

  struct Data
  {
    explicit Data(const Group::Membership& _membership)
      : membership(_membership) {}
    Group::Membership membership;
    process::Promise<Option<std::string>> promise;
  };

  struct Watch
  {
    explicit Watch(const std::set<Group::Membership>& _expected)
      : expected(_expected) {}
    std::set<Group::Membership> expected;
    process::Promise<std::set<Group::Membership>> promise;
  };

  struct {
    std::queue<Join*> joins;
    std::queue<Cancel*> cancels;
    std::queue<Data*> datas;
    std::queue<Watch*> watches;
  } pending;

  // Indicates there is a pending delayed retry.
  bool retrying;

  // Expected ZooKeeper sequence numbers (either owned/created by this
  // group instance or not) and the promise we associate with their
  // "cancellation" (i.e., no longer part of the group).
  std::map<int32_t, process::Promise<bool>*> owned;
  std::map<int32_t, process::Promise<bool>*> unowned;

  // Cache of owned + unowned, where 'None' represents an invalid
  // cache and 'Some' represents a valid cache.
  Option<std::set<Group::Membership>> memberships;

  // A timer that controls when we should give up on waiting for the
  // current connection attempt to succeed and try to reconnect.
  Option<process::Timer> connectTimer;
};

} // namespace zookeeper {

#endif // __MESOS_ZOOKEEPER_GROUP_HPP__
