#ifndef __ZOOKEEPER_GROUP_HPP__
#define __ZOOKEEPER_GROUP_HPP__

#include <set>

#include "process/future.hpp"
#include "process/option.hpp"

#include "common/seconds.hpp"

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
  // younger membership).
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

  private:
    friend class GroupProcess; // Creates and manages memberships.

    Membership(uint64_t _sequence) : sequence(_sequence) {}

    uint64_t sequence;
  };

  // Constructs this group using the specified ZooKeeper servers (list
  // of host:port) with the given timeout at the specified znode.
  Group(const std::string& servers,
        const seconds& timeout,
        const std::string& znode,
        const Option<Authentication>& auth = Option<Authentication>::none());
  ~Group();

  // Returns the result of trying to join a "group" in ZooKeeper. If
  // succesful, an "owned" membership will be returned whose
  // retrievable information will be a copy of the specified
  // parameter. A membership is not "renewed" in the event of a
  // ZooKeeper session expiration. Instead, a client should watch the
  // group memberships and rejoin the group as appropriate.
  process::Future<Membership> join(const std::string& info);

  // Returns the result of trying to cancel a membership. Note that
  // only memberships that are "owned" (see join) can be canceled.
  process::Future<bool> cancel(const Membership& membership);

  // Returns the result of trying to fetch the information associated
  // with a group membership.
  process::Future<std::string> info(const Membership& membership);

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
