
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/logging.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "common/protobuf_utils.hpp"

#include "master/constants.hpp"
#include "master/detector.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

using namespace process;
using namespace zookeeper;

using std::set;
using std::string;

namespace mesos {
namespace internal {

const Duration MASTER_DETECTOR_ZK_SESSION_TIMEOUT = Seconds(10);

// TODO(bmahler): Consider moving these kinds of helpers into
// libprocess or a common header within mesos.
namespace promises {

// Helper for setting a set of Promises.
template <typename T>
void set(std::set<Promise<T>* >* promises, const T& t)
{
  foreach (Promise<T>* promise, *promises) {
    promise->set(t);
    delete promise;
  }
  promises->clear();
}


// Helper for failing a set of Promises.
template <typename T>
void fail(std::set<Promise<T>* >* promises, const string& failure)
{
  foreach (Promise<Option<MasterInfo> >* promise, *promises) {
    promise->fail(failure);
    delete promise;
  }
  promises->clear();
}


// Helper for discarding a set of Promises.
template <typename T>
void discard(std::set<Promise<T>* >* promises)
{
  foreach (Promise<T>* promise, *promises) {
    promise->discard();
    delete promise;
  }
  promises->clear();
}


// Helper for discarding an individual promise in the set.
template <typename T>
void discard(std::set<Promise<T>* >* promises, const Future<T>& future)
{
  foreach (Promise<T>* promise, *promises) {
    if (promise->future() == future) {
      promise->discard();
      promises->erase(promise);
      delete promise;
      return;
    }
  }
}

} // namespace promises {


class StandaloneMasterDetectorProcess
  : public Process<StandaloneMasterDetectorProcess>
{
public:
  StandaloneMasterDetectorProcess()
    : ProcessBase(ID::generate("standalone-master-detector")) {}
  explicit StandaloneMasterDetectorProcess(const MasterInfo& _leader)
    : ProcessBase(ID::generate("standalone-master-detector")),
      leader(_leader) {}

  ~StandaloneMasterDetectorProcess()
  {
    promises::discard(&promises);
  }

  void appoint(const Option<MasterInfo>& leader_)
  {
    leader = leader_;

    promises::set(&promises, leader);
  }

  Future<Option<MasterInfo> > detect(
      const Option<MasterInfo>& previous = None())
  {
    if (leader != previous) {
      return leader;
    }

    Promise<Option<MasterInfo> >* promise = new Promise<Option<MasterInfo> >();

    promise->future()
      .onDiscard(defer(self(), &Self::discard, promise->future()));

    promises.insert(promise);
    return promise->future();
  }

private:
  void discard(const Future<Option<MasterInfo> >& future)
  {
    // Discard the promise holding this future.
    promises::discard(&promises, future);
  }

  Option<MasterInfo> leader; // The appointed master.
  set<Promise<Option<MasterInfo> >*> promises;
};


class ZooKeeperMasterDetectorProcess
  : public Process<ZooKeeperMasterDetectorProcess>
{
public:
  explicit ZooKeeperMasterDetectorProcess(const zookeeper::URL& url);
  explicit ZooKeeperMasterDetectorProcess(Owned<Group> group);
  ~ZooKeeperMasterDetectorProcess();

  virtual void initialize();
  Future<Option<MasterInfo> > detect(const Option<MasterInfo>& previous);

private:
  void discard(const Future<Option<MasterInfo> >& future);

  // Invoked when the group leadership has changed.
  void detected(const Future<Option<Group::Membership> >& leader);

  // Invoked when we have fetched the data associated with the leader.
  void fetched(
      const Group::Membership& membership,
      const Future<Option<string> >& data);

  Owned<Group> group;
  LeaderDetector detector;

  // The leading Master.
  Option<MasterInfo> leader;
  set<Promise<Option<MasterInfo> >*> promises;

  // Potential non-retryable error.
  Option<Error> error;
};


Try<MasterDetector*> MasterDetector::create(const string& mechanism)
{
  if (mechanism == "") {
    return new StandaloneMasterDetector();
  } else if (strings::startsWith(mechanism, "zk://")) {
    Try<zookeeper::URL> url = zookeeper::URL::parse(mechanism);
    if (url.isError()) {
      return Error(url.error());
    }
    if (url.get().path == "/") {
      return Error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterDetector(url.get());
  } else if (strings::startsWith(mechanism, "file://")) {
    // Load the configuration out of a file. While Mesos and related
    // programs always use <stout/flags> to process the command line
    // arguments (and therefore file://) this entrypoint is exposed by
    // libmesos, with frameworks currently calling it and expecting it
    // to do the argument parsing for them which roughly matches the
    // argument parsing Mesos will do.
    // TODO(cmaloney): Rework the libmesos exposed APIs to expose
    // A "flags" endpoint where the framework can pass the command
    // line arguments and they will be parsed by <stout/flags> and the
    // needed flags extracted, and then change this interface to
    // require final values from teh flags. This means that a
    // framework doesn't need to know how the flags are passed to
    // match mesos' command line arguments if it wants, but if it
    // needs to inspect/manipulate arguments, it can.
    LOG(WARNING) << "Specifying master detection mechanism / ZooKeeper URL to "
                    "be read out of a file via 'file://' is deprecated inside "
                    "Mesos and will be removed in a future release.";
    const string& path = mechanism.substr(7);
    const Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read from file at '" + path + "'");
    }

    return create(strings::trim(read.get()));
  }

  CHECK(!strings::startsWith(mechanism, "file://"));

  // Okay, try and parse what we got as a PID.
  UPID pid = mechanism.find("master@") == 0
    ? UPID(mechanism)
    : UPID("master@" + mechanism);

  if (!pid) {
    return Error("Failed to parse '" + mechanism + "'");
  }

  return new StandaloneMasterDetector(protobuf::createMasterInfo(pid));
}


MasterDetector::~MasterDetector() {}


StandaloneMasterDetector::StandaloneMasterDetector()
{
  process = new StandaloneMasterDetectorProcess();
  spawn(process);
}


StandaloneMasterDetector::StandaloneMasterDetector(const MasterInfo& leader)
{
  process = new StandaloneMasterDetectorProcess(leader);
  spawn(process);
}


StandaloneMasterDetector::StandaloneMasterDetector(const UPID& leader)
{
  process =
    new StandaloneMasterDetectorProcess(protobuf::createMasterInfo(leader));

  spawn(process);
}


StandaloneMasterDetector::~StandaloneMasterDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


void StandaloneMasterDetector::appoint(const Option<MasterInfo>& leader)
{
  dispatch(process, &StandaloneMasterDetectorProcess::appoint, leader);
}


void StandaloneMasterDetector::appoint(const UPID& leader)
{
  dispatch(process,
           &StandaloneMasterDetectorProcess::appoint,
           protobuf::createMasterInfo(leader));
}


Future<Option<MasterInfo> > StandaloneMasterDetector::detect(
    const Option<MasterInfo>& previous)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::detect, previous);
}


// TODO(benh): Get ZooKeeper timeout from configuration.
// TODO(xujyan): Use peer constructor after switching to C++ 11.
ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    const zookeeper::URL& url)
  : ProcessBase(ID::generate("zookeeper-master-detector")),
    group(new Group(url.servers,
                    MASTER_DETECTOR_ZK_SESSION_TIMEOUT,
                    url.path,
                    url.authentication)),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    Owned<Group> _group)
  : ProcessBase(ID::generate("zookeeper-master-detector")),
    group(_group),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::~ZooKeeperMasterDetectorProcess()
{
  promises::discard(&promises);
}


void ZooKeeperMasterDetectorProcess::initialize()
{
  detector.detect()
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::discard(
    const Future<Option<MasterInfo> >& future)
{
  // Discard the promise holding this future.
  promises::discard(&promises, future);
}


Future<Option<MasterInfo> > ZooKeeperMasterDetectorProcess::detect(
    const Option<MasterInfo>& previous)
{
  // Return immediately if the detector is no longer operational due
  // to a non-retryable error.
  if (error.isSome()) {
    return Failure(error.get().message);
  }

  if (leader != previous) {
    return leader;
  }

  Promise<Option<MasterInfo> >* promise = new Promise<Option<MasterInfo> >();

  promise->future()
    .onDiscard(defer(self(), &Self::discard, promise->future()));

  promises.insert(promise);
  return promise->future();
}


void ZooKeeperMasterDetectorProcess::detected(
    const Future<Option<Group::Membership> >& _leader)
{
  CHECK(!_leader.isDiscarded());

  if (_leader.isFailed()) {
    LOG(ERROR) << "Failed to detect the leader: " << _leader.failure();

    // Setting this error stops the detection loop and the detector
    // transitions to an erroneous state. Further calls to detect()
    // will directly fail as a result.
    error = Error(_leader.failure());
    leader = None();

    promises::fail(&promises, _leader.failure());

    return;
  }

  if (_leader.get().isNone()) {
    leader = None();

    promises::set(&promises, leader);
  } else {
    // Fetch the data associated with the leader.
    group->data(_leader.get().get())
      .onAny(defer(self(), &Self::fetched, _leader.get().get(), lambda::_1));
  }

  // Keep trying to detect leadership changes.
  detector.detect(_leader.get())
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::fetched(
    const Group::Membership& membership,
    const Future<Option<string> >& data)
{
  CHECK(!data.isDiscarded());

  if (data.isFailed()) {
    leader = None();
    promises::fail(&promises, data.failure());
    return;
  } else if (data.get().isNone()) {
    // Membership is gone before we can read its data.
    leader = None();
    promises::set(&promises, leader);
    return;
  }

  // Parse the data based on the membership label and cache the
  // leader for subsequent requests.
  Option<string> label = membership.label();
  if (label.isNone()) {
    // If we are here it means some masters are still creating znodes
    // with the old format.
    UPID pid = UPID(data.get().get());
    LOG(WARNING) << "Leading master " << pid << " has data in old format";
    leader = protobuf::createMasterInfo(pid);
  } else if (label.isSome() && label.get() == master::MASTER_INFO_LABEL) {
    MasterInfo info;
    if (!info.ParseFromString(data.get().get())) {
      leader = None();
      promises::fail(&promises, "Failed to parse data into MasterInfo");
      return;
    }
    leader = info;
  } else {
    leader = None();
    promises::fail(
        &promises,
        "Failed to parse data of unknown label '" + label.get() + "'");
    return;
  }

  LOG(INFO) << "A new leading master (UPID="
            << UPID(leader.get().pid()) << ") is detected";

  promises::set(&promises, leader);
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(const zookeeper::URL& url)
{
  process = new ZooKeeperMasterDetectorProcess(url);
  spawn(process);
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(Owned<Group> group)
{
  process = new ZooKeeperMasterDetectorProcess(group);
  spawn(process);
}


ZooKeeperMasterDetector::~ZooKeeperMasterDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Option<MasterInfo> > ZooKeeperMasterDetector::detect(
    const Option<MasterInfo>& previous)
{
  return dispatch(process, &ZooKeeperMasterDetectorProcess::detect, previous);
}

} // namespace internal {
} // namespace mesos {
