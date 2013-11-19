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

#include <tr1/functional>
#include <tr1/memory> // TODO(benh): Replace shared_ptr with unique_ptr.

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/logging.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "master/detector.hpp"

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


class StandaloneMasterDetectorProcess
  : public Process<StandaloneMasterDetectorProcess>
{
public:
  StandaloneMasterDetectorProcess() : leader(None()) {}
  StandaloneMasterDetectorProcess(const UPID& _leader)
    : leader(_leader) {}
  ~StandaloneMasterDetectorProcess();

  void appoint(const Result<UPID>& leader);
  Future<Result<UPID> > detect(const Result<UPID>& previous = None());

private:
  // The leading master that's directly 'appoint()'ed.
  Result<UPID> leader;

  // Promises for the detection result.
  set<Promise<Result<UPID> >*> promises;
};


class ZooKeeperMasterDetectorProcess
  : public Process<ZooKeeperMasterDetectorProcess>
{
public:
  ZooKeeperMasterDetectorProcess(const URL& url);
  ZooKeeperMasterDetectorProcess(Owned<Group> group);
  ~ZooKeeperMasterDetectorProcess();

  virtual void initialize();

  // ZooKeeperMasterDetector implementation.
  Future<Result<UPID> > detect(const Result<UPID>& previous);

private:
  // Invoked when the group leadership has changed.
  void detected(Future<Result<Group::Membership> > leader);

  // Invoked when we have fetched the data associated with the leader.
  void fetched(const Future<string>& data);

  Owned<Group> group;
  LeaderDetector detector;

  // The leading Master.
  Result<UPID> leader;
  set<Promise<Result<UPID> >*> promises;
};


Try<MasterDetector*> MasterDetector::create(const string& master)
{
  if (master == "") {
    return new StandaloneMasterDetector();
  } else if (master.find("zk://") == 0) {
    Try<URL> url = URL::parse(master);
    if (url.isError()) {
      return Try<MasterDetector*>::error(url.error());
    }
    if (url.get().path == "/") {
      return Try<MasterDetector*>::error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterDetector(url.get());
  } else if (master.find("file://") == 0) {
    const string& path = master.substr(7);
    const Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read from file at '" + path + "'");
    }

    return create(strings::trim(read.get()));
  }

  // Okay, try and parse what we got as a PID.
  UPID pid = master.find("master@") == 0
    ? UPID(master)
    : UPID("master@" + master);

  if (!pid) {
    return Try<MasterDetector*>::error(
        "Failed to parse '" + master + "'");
  }

  return new StandaloneMasterDetector(pid);
}


MasterDetector::~MasterDetector() {}


StandaloneMasterDetectorProcess::~StandaloneMasterDetectorProcess()
{
  foreach (Promise<Result<UPID> >* promise, promises) {
    promise->set(Result<UPID>::error("MasterDetector is being destructed"));
    delete promise;
  }
  promises.clear();
}


void StandaloneMasterDetectorProcess::appoint(
    const Result<process::UPID>& _leader)
{
  leader = _leader;

  foreach (Promise<Result<UPID> >* promise, promises) {
    promise->set(leader);
    delete promise;
  }
  promises.clear();
}


Future<Result<UPID> > StandaloneMasterDetectorProcess::detect(
    const Result<UPID>& previous)
{
  // Directly return the current leader is not the
  // same as the previous one.
  if (leader.isError() != previous.isError() ||
      leader.isNone() != previous.isNone() ||
      leader.isSome() != previous.isSome()) {
    return leader; // State change.
  } else if (leader.isSome() && previous.isSome() &&
             leader.get() != previous.get()) {
    return leader; // Leadership change.
  }

  Promise<Result<UPID> >* promise = new Promise<Result<UPID> >();
  promises.insert(promise);
  return promise->future();
}


StandaloneMasterDetector::StandaloneMasterDetector()
{
  process = new StandaloneMasterDetectorProcess();
  spawn(process);
}


StandaloneMasterDetector::StandaloneMasterDetector(const UPID& leader)
{
  process = new StandaloneMasterDetectorProcess(leader);
  spawn(process);
}


StandaloneMasterDetector::~StandaloneMasterDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


void StandaloneMasterDetector::appoint(const Result<process::UPID>& leader)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::appoint, leader);
}


Future<Result<UPID> > StandaloneMasterDetector::detect(
    const Result<UPID>& previous)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::detect, previous);
}


// TODO(benh): Get ZooKeeper timeout from configuration.
// TODO(xujyan): Use peer constructor after switching to C++ 11.
ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    const URL& url)
  : group(new Group(url.servers,
                    MASTER_DETECTOR_ZK_SESSION_TIMEOUT,
                    url.path,
                    url.authentication)),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    Owned<Group> _group)
  : group(_group),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::~ZooKeeperMasterDetectorProcess()
{
  foreach (Promise<Result<UPID> >* promise, promises) {
    promise->set(Result<UPID>::error("No longer detecting a master"));
    delete promise;
  }
  promises.clear();
}


void ZooKeeperMasterDetectorProcess::initialize()
{
  detector.detect(None())
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


Future<Result<UPID> > ZooKeeperMasterDetectorProcess::detect(
    const Result<UPID>& previous)
{
  // Directly return when the current leader and previous are not the
  // same.
  if (leader.isError() != previous.isError() ||
      leader.isNone() != previous.isNone() ||
      leader.isSome() != previous.isSome()) {
    return leader; // State change.
  } else if (leader.isSome() && previous.isSome() &&
             leader.get() != previous.get()) {
    return leader; // Leadership change.
  }

  Promise<Result<UPID> >* promise = new Promise<Result<UPID> >();
  promises.insert(promise);
  return promise->future();
}


void ZooKeeperMasterDetectorProcess::detected(
    Future<Result<Group::Membership> > _leader)
{
  CHECK(_leader.isReady())
    << "Not expecting LeaderDetector to fail or discard futures";

  if (!_leader.get().isSome()) {
    leader = _leader.get().isError()
        ? Result<UPID>::error(_leader.get().error())
        : Result<UPID>::none();

    foreach (Promise<Result<UPID> >* promise, promises) {
      promise->set(leader);
      delete promise;
    }
    promises.clear();
  } else {
    // Fetch the data associated with the leader.
    group->data(_leader.get().get())
      .onAny(defer(self(), &Self::fetched, lambda::_1));
  }

  // Keep trying to detect leadership changes.
  detector.detect(_leader.get())
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::fetched(const Future<string>& data)
{
  if (data.isFailed()) {
    leader = Error(data.failure());
    foreach (Promise<Result<UPID> >* promise, promises) {
      promise->set(leader);
      delete promise;
    }
    promises.clear();
    return;
  }

  CHECK(data.isReady()); // Not expecting Group to discard futures.

  // Cache the master for subsequent requests.
  leader = UPID(data.get());
  LOG(INFO) << "A new leading master (UPID=" << leader.get() << ") is detected";

  foreach (Promise<Result<UPID> >* promise, promises) {
    promise->set(leader);
    delete promise;
  }
  promises.clear();
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(const URL& url)
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


Future<Result<UPID> > ZooKeeperMasterDetector::detect(
    const Result<UPID>& previous)
{
  return dispatch(process, &ZooKeeperMasterDetectorProcess::detect, previous);
}

} // namespace internal {
} // namespace mesos {
