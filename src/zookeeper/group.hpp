#ifndef __ZOOKEEPER_GROUP_HPP__
#define __ZOOKEEPER_GROUP_HPP__

#include <set>

#include "process/future.hpp"

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

#include "zookeeper/authentication.hpp"

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
    bool operator == (const Membership& that) const
    {
      return sequence == that.sequence;
    }

    bool operator < (const Membership& that) const
    {
      return sequence < that.sequence;
    }

    bool operator <= (const Membership& that) const
    {
      return sequence <= that.sequence;
    }

    bool operator > (const Membership& that) const
    {
      return sequence > that.sequence;
    }

    bool operator >= (const Membership& that) const
    {
      return sequence >= that.sequence;
    }

    uint64_t id() const
    {
      return sequence;
    }

    // Returns a future that is only satisfied once this membership
    // has been cancelled. In which case, the value of the future is
    // true if you own this membership and cancelled it by invoking
    // Group::cancel. Otherwise, the value of the future is false (and
    // could signify cancellation due to a sesssion expiration or
    // operator error).
    process::Future<bool> cancelled() const
    {
      return cancelled_;
    }

  private:
    friend class GroupProcess; // Creates and manages memberships.

    Membership(uint64_t _sequence, const process::Future<bool>& cancelled)
      : sequence(_sequence), cancelled_(cancelled) {}

    uint64_t sequence;
    process::Future<bool> cancelled_;
  };

  // Constructs this group using the specified ZooKeeper servers (list
  // of host:port) with the given timeout at the specified znode.
  Group(const std::string& servers,
        const Duration& timeout,
        const std::string& znode,
        const Option<Authentication>& auth = None());
  ~Group();

  // Returns the result of trying to join a "group" in ZooKeeper. If
  // succesful, an "owned" membership will be returned whose
  // retrievable data will be a copy of the specified parameter. A
  // membership is not "renewed" in the event of a ZooKeeper session
  // expiration. Instead, a client should watch the group memberships
  // and rejoin the group as appropriate.
  process::Future<Membership> join(const std::string& data);

  // Returns the result of trying to cancel a membership. Note that
  // only memberships that are "owned" (see join) can be canceled.
  process::Future<bool> cancel(const Membership& membership);

  // Returns the result of trying to fetch the data associated with a
  // group membership.
  process::Future<std::string> data(const Membership& membership);

  // Returns a future that gets set when the group memberships differ
  // from the "expected" memberships specified.
  process::Future<std::set<Membership> > watch(
      const std::set<Membership>& expected = std::set<Membership>());

  // Returns the current ZooKeeper session associated with this group,
  // or none if no session currently exists.
  process::Future<Option<int64_t> > session();

private:
  GroupProcess* process;
};

} // namespace zookeeper {

#endif // __ZOOKEEPER_GROUP_HPP__
