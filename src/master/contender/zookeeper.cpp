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

#include "master/contender/zookeeper.hpp"

#include <mesos/master/contender.hpp>

#include <mesos/zookeeper/contender.hpp>
#include <mesos/zookeeper/group.hpp>
#include <mesos/zookeeper/url.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/protobuf.hpp>

#include "master/constants.hpp"

using std::string;

using namespace process;
using namespace zookeeper;

namespace mesos {
namespace master {
namespace contender {

const Duration MASTER_CONTENDER_ZK_SESSION_TIMEOUT = Seconds(10);


class ZooKeeperMasterContenderProcess
  : public Process<ZooKeeperMasterContenderProcess>
{
public:
  explicit ZooKeeperMasterContenderProcess(
      const zookeeper::URL& url,
      const Duration& sessionTimeout);

  explicit ZooKeeperMasterContenderProcess(Owned<zookeeper::Group> group);
  ~ZooKeeperMasterContenderProcess() override;

  // Explicitly use 'initialize' since we're overloading below.
  using process::ProcessBase::initialize;

  void initialize(const MasterInfo& masterInfo);

  // MasterContender implementation.
  virtual Future<Future<Nothing>> contend();

private:
  Owned<zookeeper::Group> group;
  LeaderContender* contender;

  // The master this contender contends on behalf of.
  Option<MasterInfo> masterInfo;
  Option<Future<Future<Nothing>>> candidacy;
};


ZooKeeperMasterContender::ZooKeeperMasterContender(
    const zookeeper::URL& url,
    const Duration& sessionTimeout)
{
  process = new ZooKeeperMasterContenderProcess(url, sessionTimeout);
  spawn(process);
}


ZooKeeperMasterContender::ZooKeeperMasterContender(Owned<Group> group)
{
  process = new ZooKeeperMasterContenderProcess(group);
  spawn(process);
}


ZooKeeperMasterContender::~ZooKeeperMasterContender()
{
  terminate(process);
  process::wait(process);
  delete process;
}


void ZooKeeperMasterContender::initialize(const MasterInfo& masterInfo)
{
  process->initialize(masterInfo);
}


Future<Future<Nothing>> ZooKeeperMasterContender::contend()
{
  return dispatch(process, &ZooKeeperMasterContenderProcess::contend);
}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    const zookeeper::URL& url,
    const Duration& sessionTimeout)
  : ZooKeeperMasterContenderProcess(Owned<Group>(
    new Group(url, sessionTimeout))) {}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    Owned<Group> _group)
  : ProcessBase(ID::generate("zookeeper-master-contender")),
    group(_group),
    contender(nullptr) {}


ZooKeeperMasterContenderProcess::~ZooKeeperMasterContenderProcess()
{
  delete contender;
}


void ZooKeeperMasterContenderProcess::initialize(const MasterInfo& _masterInfo)
{
  masterInfo = _masterInfo;
}


Future<Future<Nothing>> ZooKeeperMasterContenderProcess::contend()
{
  if (masterInfo.isNone()) {
    return Failure("Initialize the contender first");
  }

  // Should not recontend if the last election is still ongoing.
  if (candidacy.isSome() && candidacy->isPending()) {
    return candidacy.get();
  }

  if (contender != nullptr) {
    LOG(INFO) << "Withdrawing the previous membership before recontending";
    delete contender;
  }

  // Serialize the MasterInfo to JSON.
  JSON::Object json = JSON::protobuf(masterInfo.get());

  contender = new LeaderContender(
      group.get(),
      stringify(json),
      mesos::internal::master::MASTER_INFO_JSON_LABEL);
  candidacy = contender->contend();
  return candidacy.get();
}

} // namespace contender {
} // namespace master {
} // namespace mesos {
