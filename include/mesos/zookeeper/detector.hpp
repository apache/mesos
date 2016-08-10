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

#ifndef __MESOS_ZOOKEEPER_DETECTOR_HPP__
#define __MESOS_ZOOKEEPER_DETECTOR_HPP__

#include <string>

#include <mesos/zookeeper/group.hpp>

#include <stout/result.hpp>

#include <process/future.hpp>

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
  process::Future<Option<Group::Membership>> detect(
      const Option<Group::Membership>& previous = None());

private:
  LeaderDetectorProcess* process;
};

} // namespace zookeeper {

#endif // __MESOS_ZOOKEEPER_DETECTOR_HPP__
