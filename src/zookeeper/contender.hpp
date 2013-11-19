#ifndef __ZOOKEEPER_CONTENDER_HPP
#define __ZOOKEEPER_CONTENDER_HPP

#include <string>

#include <process/future.hpp>

#include <stout/nothing.hpp>

#include "zookeeper/group.hpp"

namespace zookeeper {

// Forward declaration.
class LeaderContenderProcess;


// Provides an abstraction for contending to be the leader of a
// ZooKeeper group.
// Note that the contender is NOT reusable, which means its methods
// are supposed to be called once and the client needs to create a
// new instance to contend again.
class LeaderContender
{
public:
  // The specified 'group' is expected to outlive the contender. The
  // specified 'data' is associated with the group membership created
  // by this contender.
  LeaderContender(Group* group, const std::string& data);

  // Note that the contender's membership, if obtained, is scheduled
  // to be cancelled during destruction.
  // NOTE: The client should call withdraw() to guarantee that the
  // membership is cancelled when its returned future is satisfied.
  virtual ~LeaderContender();

  // Returns a Future<Nothing> once the contender has achieved
  // candidacy (by obtaining a membership) and a failure otherwise.
  // The inner Future returns Nothing when the contender is out of
  // the contest (i.e. its membership is lost) and a failure if it is
  // unable to watch the membership.
  process::Future<process::Future<Nothing> > contend();

  // Returns true if successfully withdrawn from the contest (either
  // while contending or has already contended and is watching for
  // membership loss).
  // It should only be called after contend() is called.
  // A false return value implies that there was no valid group
  // membership to cancel, which may be a result of a race to cancel
  // an expired membership.
  // A failed future is returned if the contender is unable to
  // withdraw (but the membership will expire).
  process::Future<bool> withdraw();

private:
  LeaderContenderProcess* process;
};

} // namespace zookeeper {

#endif // __ZOOKEEPER_CONTENDER_HPP
