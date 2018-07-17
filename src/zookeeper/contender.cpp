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

#include <string>

#include <glog/logging.h>

#include <mesos/zookeeper/contender.hpp>
#include <mesos/zookeeper/detector.hpp>
#include <mesos/zookeeper/group.hpp>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

using std::string;

using process::Failure;
using process::Future;
using process::Process;
using process::Promise;

namespace zookeeper {

class LeaderContenderProcess : public Process<LeaderContenderProcess>
{
public:
  LeaderContenderProcess(
      Group* group,
      const string& data,
      const Option<string>& label);

  ~LeaderContenderProcess() override;

  // LeaderContender implementation.
  Future<Future<Nothing>> contend();
  Future<bool> withdraw();

protected:
  void finalize() override;

private:
  // Invoked when we have joined the group (or failed to do so).
  void joined();

  // Invoked when the group membership is cancelled.
  void cancelled(const Future<bool>& result);

  // Helper for cancelling the Group membership.
  void cancel();

  Group* group;
  const string data;
  const Option<string> label;

  // The contender's state transitions from contending -> watching ->
  // withdrawing or contending -> withdrawing. Each state is
  // identified by the corresponding Option<Promise> being assigned.
  // Note that these Option<Promise>s are never reset to None once it
  // is assigned.

  // Holds the promise for the future for contend().
  Option<Promise<Future<Nothing>>*> contending;

  // Holds the promise for the inner future enclosed by contend()'s
  // result which is satisfied when the contender's candidacy is
  // lost.
  Option<Promise<Nothing>*> watching;

  // Holds the promise for the future for withdraw().
  Option<Promise<bool>*> withdrawing;

  // Stores the result for joined().
  Future<Group::Membership> candidacy;
};


LeaderContenderProcess::LeaderContenderProcess(
    Group* _group,
    const string& _data,
    const Option<string>& _label)
  : ProcessBase(process::ID::generate("zookeeper-leader-contender")),
    group(_group),
    data(_data),
    label(_label) {}


LeaderContenderProcess::~LeaderContenderProcess()
{
  if (contending.isSome()) {
    contending.get()->discard();
    delete contending.get();
    contending = None();
  }

  if (watching.isSome()) {
    watching.get()->discard();
    delete watching.get();
    watching = None();
  }

  if (withdrawing.isSome()) {
    withdrawing.get()->discard();
    delete withdrawing.get();
    withdrawing = None();
  }
}


void LeaderContenderProcess::finalize()
{
  // We do not wait for the result here because the Group keeps
  // retrying (even after the contender is destroyed) until it
  // succeeds so the old membership is eventually going to be
  // cancelled.
  // There is a tricky situation where the contender terminates after
  // it has contended but before it is notified of the obtained
  // membership. In this case the membership is not cancelled during
  // contender destruction. The client thus should use withdraw() to
  // wait for the membership to be first obtained and then cancelled.
  cancel();
}


Future<Future<Nothing>> LeaderContenderProcess::contend()
{
  if (contending.isSome()) {
    return Failure("Cannot contend more than once");
  }

  LOG(INFO) << "Joining the ZK group";
  candidacy = group->join(data, label);
  candidacy
    .onAny(defer(self(), &Self::joined));

  // Okay, we wait and see what unfolds.
  contending = new Promise<Future<Nothing>>();
  return contending.get()->future();
}


Future<bool> LeaderContenderProcess::withdraw()
{
  if (contending.isNone()) {
    // Nothing to withdraw because the contender has not contended.
    return false;
  }

  if (withdrawing.isSome()) {
    // Repeated calls to withdraw get the same result.
    return withdrawing.get();
  }

  withdrawing = new Promise<bool>();

  CHECK(!candidacy.isDiscarded());

  if (candidacy.isPending()) {
    // If we have not obtained the candidacy yet, we withdraw after
    // it is obtained.
    LOG(INFO) << "Withdraw requested before the candidacy is obtained; will "
              << "withdraw after it happens";
    candidacy.onAny(defer(self(), &Self::cancel));
  } else if (candidacy.isReady()) {
    cancel();
  } else {
    // We have failed to obtain the candidacy so we do not need to
    // cancel it.
    return false;
  }

  return withdrawing.get()->future();
}


void LeaderContenderProcess::cancel()
{
  if (!candidacy.isReady()) {
    // Nothing to cancel.
    if (withdrawing.isSome()) {
      withdrawing.get()->set(false);
    }
    return;
  }

  LOG(INFO) << "Now cancelling the membership: " << candidacy->id();

  group->cancel(candidacy.get())
    .onAny(defer(self(), &Self::cancelled, lambda::_1));
}


void LeaderContenderProcess::cancelled(const Future<bool>& result)
{
  CHECK_READY(candidacy);
  LOG(INFO) << "Membership cancelled: " << candidacy->id();

  // Can be called as a result of either withdraw() or server side
  // expiration.
  CHECK(withdrawing.isSome() || watching.isSome());

  CHECK(!result.isDiscarded());

  if (result.isFailed()) {
    if (withdrawing.isSome()) {
      withdrawing.get()->fail(result.failure());
    }

    if (watching.isSome()) {
      watching.get()->fail(result.failure());
    }
  } else {
    if (withdrawing.isSome()) {
      withdrawing.get()->set(result);
    }

    if (watching.isSome()) {
      watching.get()->set(Nothing());
    }
  }
}


void LeaderContenderProcess::joined()
{
  CHECK(!candidacy.isDiscarded());

  // Cannot be watching because the candidacy is not obtained yet.
  CHECK_NONE(watching);

  CHECK_SOME(contending);

  if (candidacy.isFailed()) {
    // The promise 'withdrawing' will be set to false in cancel().
    contending.get()->fail(candidacy.failure());
    return;
  }

  if (withdrawing.isSome()) {
    LOG(INFO) << "Joined group after the contender started withdrawing";

    // The promise 'withdrawing' will be set to 'false' in subsequent
    // 'cancel()' call.
    return;
  }

  LOG(INFO) << "New candidate (id='" << candidacy->id()
            << "') has entered the contest for leadership";

  // Transition to 'watching' state.
  watching = new Promise<Nothing>();

  // Notify the client.
  if (contending.get()->set(watching.get()->future())) {
    // Continue to watch that our membership is not removed (if the
    // client still cares about it).
    candidacy->cancelled()
      .onAny(defer(self(), &Self::cancelled, lambda::_1));
  }
}


LeaderContender::LeaderContender(
    Group* group,
    const string& data,
    const Option<string>& label)
{
  process = new LeaderContenderProcess(group, data, label);
  spawn(process);
}


LeaderContender::~LeaderContender()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Future<Nothing>> LeaderContender::contend()
{
  return dispatch(process, &LeaderContenderProcess::contend);
}


Future<bool> LeaderContender::withdraw()
{
  return dispatch(process, &LeaderContenderProcess::withdraw);
}

} // namespace zookeeper {
