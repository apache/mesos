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

#include <set>

#include <mesos/zookeeper/detector.hpp>
#include <mesos/zookeeper/group.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include "process/logging.hpp"
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

using namespace process;

using std::set;

namespace zookeeper {

class LeaderDetectorProcess : public Process<LeaderDetectorProcess>
{
public:
  explicit LeaderDetectorProcess(Group* group);
  ~LeaderDetectorProcess() override;
  void initialize() override;

  // LeaderDetector implementation.
  Future<Option<Group::Membership>> detect(
      const Option<Group::Membership>& previous);

private:
  // Helper that sets up the watch on the group.
  void watch(const set<Group::Membership>& expected);

  // Invoked when the group memberships have changed.
  void watched(const Future<set<Group::Membership>>& memberships);

  Group* group;
  Option<Group::Membership> leader;
  set<Promise<Option<Group::Membership>>*> promises;

  // Potential non-retryable error.
  Option<Error> error;
};


LeaderDetectorProcess::LeaderDetectorProcess(Group* _group)
  : ProcessBase(ID::generate("zookeeper-leader-detector")),
    group(_group),
    leader(None()) {}


LeaderDetectorProcess::~LeaderDetectorProcess()
{
  foreach (Promise<Option<Group::Membership>>* promise, promises) {
    promise->future().discard();
    delete promise;
  }
  promises.clear();
}


void LeaderDetectorProcess::initialize()
{
  watch(set<Group::Membership>());
}


Future<Option<Group::Membership>> LeaderDetectorProcess::detect(
    const Option<Group::Membership>& previous)
{
  // Return immediately if the detector is no longer operational due
  // to the non-retryable error.
  if (error.isSome()) {
    return Failure(error->message);
  }

  // Return immediately if the incumbent leader is different from the
  // expected.
  if (leader != previous) {
    return leader;
  }

  // Otherwise wait for the next election result.
  Promise<Option<Group::Membership>>* promise =
    new Promise<Option<Group::Membership>>();
  promises.insert(promise);
  return promise->future();
}


void LeaderDetectorProcess::watch(const set<Group::Membership>& expected)
{
  group->watch(expected)
    .onAny(defer(self(), &Self::watched, lambda::_1));
}


void LeaderDetectorProcess::watched(
    const Future<set<Group::Membership>>& memberships)
{
  CHECK(!memberships.isDiscarded());

  if (memberships.isFailed()) {
    LOG(ERROR) << "Failed to watch memberships: " << memberships.failure();

    // Setting this error stops the watch loop and the detector
    // transitions to an erroneous state. Further calls to detect()
    // will directly fail as a result.
    error = Error(memberships.failure());
    leader = None();
    foreach (Promise<Option<Group::Membership>>* promise, promises) {
      promise->fail(memberships.failure());
      delete promise;
    }
    promises.clear();
    return;
  }

  // Update leader status based on memberships.
  if (leader.isSome() && memberships->count(leader.get()) == 0) {
    VLOG(1) << "The current leader (id=" << leader->id() << ") is lost";
  }

  // Run an "election". The leader is the oldest member (smallest
  // membership id). We do not fulfill any of our promises if the
  // incumbent wins the election.
  Option<Group::Membership> current;
  foreach (const Group::Membership& membership, memberships.get()) {
    current = min(current, membership);
  }

  if (current != leader) {
    LOG(INFO) << "Detected a new leader: "
              << (current.isSome()
                  ? "(id='" + stringify(current->id()) + "')"
                  : "None");

    foreach (Promise<Option<Group::Membership>>* promise, promises) {
      promise->set(current);
      delete promise;
    }
    promises.clear();
  }

  leader = current;
  watch(memberships.get());
}


LeaderDetector::LeaderDetector(Group* group)
{
  process = new LeaderDetectorProcess(group);
  spawn(process);
}


LeaderDetector::~LeaderDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Option<Group::Membership>> LeaderDetector::detect(
    const Option<Group::Membership>& membership)
{
  return dispatch(process, &LeaderDetectorProcess::detect, membership);
}

} // namespace zookeeper {
