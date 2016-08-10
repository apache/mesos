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

#ifndef __MESOS_ZOOKEEPER_CONTENDER_HPP__
#define __MESOS_ZOOKEEPER_CONTENDER_HPP__

#include <string>

#include <mesos/zookeeper/group.hpp>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>

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
  // by this contender. 'label' indicates the label for the znode that
  // stores the 'data'.
  LeaderContender(Group* group,
                  const std::string& data,
                  const Option<std::string>& label);

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
  // It should be called only once, otherwise a failure is returned.
  process::Future<process::Future<Nothing>> contend();

  // Returns true if successfully withdrawn from the contest (either
  // while contending or has already contended and is watching for
  // membership loss).
  // A false return value implies that there was no valid group
  // membership to cancel, which may be a result of a race to cancel
  // an expired membership or because there is nothing to withdraw.
  // A failed future is returned if the contender is unable to
  // withdraw.
  process::Future<bool> withdraw();

private:
  LeaderContenderProcess* process;
};

} // namespace zookeeper {

#endif // __MESOS_ZOOKEEPER_CONTENDER_HPP__
