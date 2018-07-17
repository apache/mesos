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
// limitations under the License.

#include "master/detector/zookeeper.hpp"

#include <set>
#include <string>

#include <mesos/master/detector.hpp>

#include <mesos/zookeeper/detector.hpp>
#include <mesos/zookeeper/group.hpp>
#include <mesos/zookeeper/url.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/protobuf.hpp>

#include "common/protobuf_utils.hpp"

#include "master/constants.hpp"

using namespace process;
using namespace zookeeper;

using std::set;
using std::string;

namespace mesos {
namespace master {
namespace detector {

const Duration MASTER_DETECTOR_ZK_SESSION_TIMEOUT = Seconds(10);

class ZooKeeperMasterDetectorProcess
  : public Process<ZooKeeperMasterDetectorProcess>
{
public:
  explicit ZooKeeperMasterDetectorProcess(
      const zookeeper::URL& url,
      const Duration& sessionTimeout);

  explicit ZooKeeperMasterDetectorProcess(Owned<Group> group);
  ~ZooKeeperMasterDetectorProcess() override;

  void initialize() override;
  Future<Option<MasterInfo>> detect(const Option<MasterInfo>& previous);

private:
  void discard(const Future<Option<MasterInfo>>& future);

  // Invoked when the group leadership has changed.
  void detected(const Future<Option<Group::Membership>>& leader);

  // Invoked when we have fetched the data associated with the leader.
  void fetched(
      const Group::Membership& membership,
      const Future<Option<string>>& data);

  Owned<Group> group;
  LeaderDetector detector;

  // The leading Master.
  Option<MasterInfo> leader;
  set<Promise<Option<MasterInfo>>*> promises;

  // Potential non-retryable error.
  Option<Error> error;
};


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    const zookeeper::URL& url,
    const Duration& sessionTimeout)
  : ZooKeeperMasterDetectorProcess(Owned<Group>(
    new Group(url.servers,
              sessionTimeout,
              url.path,
              url.authentication))) {}


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    Owned<Group> _group)
  : ProcessBase(ID::generate("zookeeper-master-detector")),
    group(_group),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::~ZooKeeperMasterDetectorProcess()
{
  discardPromises(&promises);
}


void ZooKeeperMasterDetectorProcess::initialize()
{
  detector.detect()
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::discard(
    const Future<Option<MasterInfo>>& future)
{
  // Discard the promise holding this future.
  discardPromises(&promises, future);
}


Future<Option<MasterInfo>> ZooKeeperMasterDetectorProcess::detect(
    const Option<MasterInfo>& previous)
{
  // Return immediately if the detector is no longer operational due
  // to a non-retryable error.
  if (error.isSome()) {
    return Failure(error->message);
  }

  if (leader != previous) {
    return leader;
  }

  Promise<Option<MasterInfo>>* promise = new Promise<Option<MasterInfo>>();

  promise->future()
    .onDiscard(defer(self(), &Self::discard, promise->future()));

  promises.insert(promise);
  return promise->future();
}


void ZooKeeperMasterDetectorProcess::detected(
    const Future<Option<Group::Membership>>& _leader)
{
  CHECK(!_leader.isDiscarded());

  if (_leader.isFailed()) {
    LOG(ERROR) << "Failed to detect the leader: " << _leader.failure();

    // Setting this error stops the detection loop and the detector
    // transitions to an erroneous state. Further calls to detect()
    // will directly fail as a result.
    error = Error(_leader.failure());
    leader = None();

    failPromises(&promises, _leader.failure());

    return;
  }

  if (_leader->isNone()) {
    leader = None();

    setPromises(&promises, leader);
  } else {
    // Fetch the data associated with the leader.
    group->data(_leader->get())
      .onAny(defer(self(), &Self::fetched, _leader->get(), lambda::_1));
  }

  // Keep trying to detect leadership changes.
  detector.detect(_leader.get())
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::fetched(
    const Group::Membership& membership,
    const Future<Option<string>>& data)
{
  CHECK(!data.isDiscarded());

  if (data.isFailed()) {
    leader = None();
    failPromises(&promises, data.failure());
    return;
  } else if (data->isNone()) {
    // Membership is gone before we can read its data.
    leader = None();
    setPromises(&promises, leader);
    return;
  }

  // Parse the data based on the membership label and cache the
  // leader for subsequent requests.
  Option<string> label = membership.label();
  if (label.isNone()) {
    // If we are here it means some masters are still creating znodes
    // with the old format.
    UPID pid = UPID(data->get());
    LOG(WARNING) << "Leading master " << pid << " has data in old format";
    leader = mesos::internal::protobuf::createMasterInfo(pid);
  } else if (label.isSome() &&
             label.get() == internal::master::MASTER_INFO_LABEL) {
    MasterInfo info;
    if (!info.ParseFromString(data->get())) {
      leader = None();
      failPromises(&promises,
          "Failed to parse data into MasterInfo");
      return;
    }
    LOG(WARNING) << "Leading master " << info.pid()
                 << " is using a Protobuf binary format when registering with "
                 << "ZooKeeper (" << label.get() << "): this will be deprecated"
                 << " as of Mesos 0.24 (see MESOS-2340)";
    leader = info;
  } else if (label.isSome() &&
             label.get() == internal::master::MASTER_INFO_JSON_LABEL) {
    Try<JSON::Object> object = JSON::parse<JSON::Object>(data->get());

    if (object.isError()) {
      leader = None();
      failPromises(
          &promises,
          "Failed to parse data into valid JSON: " + object.error());
      return;
    }

    Try<mesos::MasterInfo> info =
      ::protobuf::parse<mesos::MasterInfo>(object.get());

    if (info.isError()) {
      leader = None();
      failPromises(
          &promises,
          "Failed to parse JSON into a valid MasterInfo protocol buffer: " +
          info.error());
      return;
    }

    leader = info.get();
  } else {
    leader = None();
    failPromises(
        &promises,
        "Failed to parse data of unknown label '" + label.get() + "'");
    return;
  }

  LOG(INFO) << "A new leading master (UPID="
            << UPID(leader->pid()) << ") is detected";

  setPromises(&promises, leader);
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(
    const zookeeper::URL& url,
    const Duration& sessionTimeout)
{
  process = new ZooKeeperMasterDetectorProcess(url, sessionTimeout);
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


Future<Option<MasterInfo>> ZooKeeperMasterDetector::detect(
    const Option<MasterInfo>& previous)
{
  return dispatch(process, &ZooKeeperMasterDetectorProcess::detect, previous);
}

} // namespace detector {
} // namespace master {
} // namespace mesos {
