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

#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/lambda.hpp>

#include "master/constants.hpp"
#include "master/contender.hpp"
#include "master/master.hpp"

#include "zookeeper/contender.hpp"
#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

using std::string;

using namespace process;
using namespace zookeeper;

namespace mesos {

using namespace master;

const Duration MASTER_CONTENDER_ZK_SESSION_TIMEOUT = Seconds(10);


class ZooKeeperMasterContenderProcess
  : public Process<ZooKeeperMasterContenderProcess>
{
public:
  explicit ZooKeeperMasterContenderProcess(const zookeeper::URL& url);
  explicit ZooKeeperMasterContenderProcess(Owned<zookeeper::Group> group);
  virtual ~ZooKeeperMasterContenderProcess();

  // Explicitely use 'initialize' since we're overloading below.
  using process::ProcessBase::initialize;

  void initialize(const MasterInfo& masterInfo);

  // MasterContender implementation.
  virtual Future<Future<Nothing> > contend();

private:
  Owned<zookeeper::Group> group;
  LeaderContender* contender;

  // The master this contender contends on behalf of.
  Option<MasterInfo> masterInfo;
  Option<Future<Future<Nothing> > > candidacy;
};


Try<MasterContender*> MasterContender::create(const string& mechanism)
{
  if (mechanism == "") {
    return new StandaloneMasterContender();
  } else if (strings::startsWith(mechanism, "zk://")) {
    Try<zookeeper::URL> url = zookeeper::URL::parse(mechanism);
    if (url.isError()) {
      return Error(url.error());
    }
    if (url.get().path == "/") {
      return Error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterContender(url.get());
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
    LOG(WARNING) << "Specifying master election mechanism / ZooKeeper URL to "
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

  return Error("Failed to parse '" + mechanism + "'");
}


MasterContender::~MasterContender() {}


StandaloneMasterContender::~StandaloneMasterContender()
{
  if (promise != NULL) {
    promise->set(Nothing()); // Leadership lost.
    delete promise;
  }
}


void StandaloneMasterContender::initialize(const MasterInfo& masterInfo)
{
  // We don't really need to store the master in this basic
  // implementation so we just restore an 'initialized' flag to make
  // sure it is called.
  initialized = true;
}


Future<Future<Nothing> > StandaloneMasterContender::contend()
{
  if (!initialized) {
    return Failure("Initialize the contender first");
  }

  if (promise != NULL) {
    LOG(INFO) << "Withdrawing the previous membership before recontending";
    promise->set(Nothing());
    delete promise;
  }

  // Directly return a future that is always pending because it
  // represents a membership/leadership that is not going to be lost
  // until we 'withdraw'.
  promise = new Promise<Nothing>();
  return promise->future();
}


ZooKeeperMasterContender::ZooKeeperMasterContender(const zookeeper::URL& url)
{
  process = new ZooKeeperMasterContenderProcess(url);
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


Future<Future<Nothing> > ZooKeeperMasterContender::contend()
{
  return dispatch(process, &ZooKeeperMasterContenderProcess::contend);
}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    const zookeeper::URL& url)
  : ProcessBase(ID::generate("zookeeper-master-contender")),
    group(new Group(url, MASTER_CONTENDER_ZK_SESSION_TIMEOUT)),
    contender(NULL) {}


ZooKeeperMasterContenderProcess::ZooKeeperMasterContenderProcess(
    Owned<Group> _group)
  : ProcessBase(ID::generate("zookeeper-master-contender")),
    group(_group),
    contender(NULL) {}


ZooKeeperMasterContenderProcess::~ZooKeeperMasterContenderProcess()
{
  delete contender;
}

void ZooKeeperMasterContenderProcess::initialize(const MasterInfo& _masterInfo)
{
  masterInfo = _masterInfo;
}


Future<Future<Nothing> > ZooKeeperMasterContenderProcess::contend()
{
  if (masterInfo.isNone()) {
    return Failure("Initialize the contender first");
  }

  // Should not recontend if the last election is still ongoing.
  if (candidacy.isSome() && candidacy.get().isPending()) {
    return candidacy.get();
  }

  if (contender != NULL) {
    LOG(INFO) << "Withdrawing the previous membership before recontending";
    delete contender;
  }

  // Serialize the MasterInfo to string.
  string data;
  if (!masterInfo.get().SerializeToString(&data)) {
    return Failure("Failed to serialize data to MasterInfo");
  }

  contender = new LeaderContender(group.get(), data, master::MASTER_INFO_LABEL);
  candidacy = contender->contend();
  return candidacy.get();
}

} // namespace mesos {
