#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include "process/logging.hpp"
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"

using namespace process;

using std::set;
using std::string;

namespace zookeeper {

class LeaderDetectorProcess : public Process<LeaderDetectorProcess>
{
public:
  LeaderDetectorProcess(Group* group);
  virtual ~LeaderDetectorProcess();
  virtual void initialize();

  // LeaderDetector implementation.
  Future<Result<Group::Membership> > detect(
      const Result<Group::Membership>& previous);

private:
  // Helper that sets up the watch on the group.
  void watch(const set<Group::Membership>& expected);

  // Invoked when the group memberships have changed.
  void watched(Future<set<Group::Membership> > memberships);

  Group* group;
  Result<Group::Membership> leader;
  set<Promise<Result<Group::Membership> >*> promises;
};


LeaderDetectorProcess::LeaderDetectorProcess(Group* _group)
  : group(_group), leader(None()) {}


LeaderDetectorProcess::~LeaderDetectorProcess()
{
  foreach (Promise<Result<Group::Membership> >* promise, promises) {
    promise->fail("No longer detecting leader");
    delete promise;
  }
  promises.clear();
}


void LeaderDetectorProcess::initialize()
{
  watch(set<Group::Membership>());
}


Future<Result<Group::Membership> > LeaderDetectorProcess::detect(
    const Result<Group::Membership>& previous)
{
  // Return immediately if the incumbent leader is different from the
  // expected.
  if (leader.isError() != previous.isError() ||
      leader.isNone() != previous.isNone() ||
      leader.isSome() != previous.isSome()) {
    return leader; // State change.
  } else if (leader.isSome() && previous.isSome() &&
             leader.get() != previous.get()) {
    return leader; // Leadership change.
  }

  // Otherwise wait for the next election result.
  Promise<Result<Group::Membership> >* promise =
    new Promise<Result<Group::Membership> >();
  promises.insert(promise);
  return promise->future();
}


void LeaderDetectorProcess::watch(const set<Group::Membership>& expected)
{
  group->watch(expected)
    .onAny(defer(self(), &Self::watched, lambda::_1));
}


void LeaderDetectorProcess::watched(Future<set<Group::Membership> > memberships)
{
  if (memberships.isFailed()) {
    LOG(ERROR) << "Failed to watch memberships: " << memberships.failure();
    leader = Error(memberships.failure());
    foreach (Promise<Result<Group::Membership> >* promise, promises) {
      promise->set(leader);
      delete promise;
    }
    promises.clear();

    // Start over.
    watch(set<Group::Membership>());
    return;
  }

  CHECK(memberships.isReady()) << "Not expecting Group to discard futures";

  // Update leader status based on memberships.
  if (leader.isSome() && memberships.get().count(leader.get()) == 0) {
    VLOG(1) << "The current leader (id=" << leader.get().id() << ") is lost";
  }

  // Run an "election". The leader is the oldest member (smallest
  // membership id). We do not fulfill any of our promises if the
  // incumbent wins the election.
  Option<Group::Membership> current;
  foreach (const Group::Membership& membership, memberships.get()) {
    if (current.isNone() || membership.id() < current.get().id()) {
      current = membership;
    }
  }

  if (current.isSome() && (!leader.isSome() || current.get() != leader.get())) {
    LOG(INFO) << "Detected a new leader (id='" << current.get().id()
              << "')";
    foreach (Promise<Result<Group::Membership> >* promise, promises) {
      promise->set(current);
      delete promise;
    }
    promises.clear();
  } else if (current.isNone() && !leader.isNone()) {
    LOG(INFO) << "No new leader is elected after election";

    foreach (Promise<Result<Group::Membership> >* promise, promises) {
      promise->set(Result<Group::Membership>::none());
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


Future<Result<Group::Membership> > LeaderDetector::detect(
    const Result<Group::Membership>& membership)
{
  return dispatch(process, &LeaderDetectorProcess::detect, membership);
}

} // namespace zookeeper {
