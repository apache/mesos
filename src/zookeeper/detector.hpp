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
  LeaderDetector(Group* group);
  virtual ~LeaderDetector();

  // Returns some membership after an election has occurred and a
  // leader (membership) is elected, or none if an election occurs and
  // no leader is elected (e.g., all memberships are lost).
  // The result is an error if the detector is not able to detect the
  // leader, possibly due to network disconnection.
  //
  // The 'previous' result (if any) should be passed back if this
  // method is called repeatedly so the detector only returns when it
  // gets a different result, either because an error is recovered or
  // the elected membership is different from the 'previous'.
  //
  // TODO(xujyan): Use a Stream abstraction instead.
  process::Future<Result<Group::Membership> > detect(
      const Result<Group::Membership>& previous = None());

private:
  LeaderDetectorProcess* process;
};

} // namespace zookeeper {

#endif // __ZOOKEEPER_DETECTOR_HPP__
