#ifndef __ZOOKEEPER_DETECTOR_HPP__
#define __ZOOKEEPER_DETECTOR_HPP__

#include <string>

#include <stout/result.hpp>

#include <process/future.hpp>

#include "zookeeper/group.hpp"

namespace zookeeper {

// Forward declaration.
class LeaderDetectorProcess;

// Provides an abstraction for detecting the leader of a ZooKeeper
// group.
class LeaderDetector
{
public:
  // The specified 'group' is expected to outlive the detector.
  explicit LeaderDetector(Group* group);
  virtual ~LeaderDetector();

  // Returns some membership after an election has occurred and a
  // leader (membership) is elected, or none if an election occurs and
  // no leader is elected (e.g., all memberships are lost).
  // A failed future is returned if the detector is unable to detect
  // the leading master due to a non-retryable error.
  // Note that the detector transparently tries to recover from
  // retryable errors until the group session expires, in which case
  // the Future returns None.
  // The future is never discarded unless it stays pending when the
  // detector destructs.
  //
  // The 'previous' result (if any) should be passed back if this
  // method is called repeatedly so the detector only returns when it
  // gets a different result.
  //
  // TODO(xujyan): Use a Stream abstraction instead.
  process::Future<Option<Group::Membership> > detect(
      const Option<Group::Membership>& previous = None());

private:
  LeaderDetectorProcess* process;
};

} // namespace zookeeper {

#endif // __ZOOKEEPER_DETECTOR_HPP__
