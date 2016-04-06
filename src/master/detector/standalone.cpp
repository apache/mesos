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

#include <set>
#include <string>

#include <mesos/master/detector.hpp>

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
#include <stout/protobuf.hpp>

#include "common/protobuf_utils.hpp"

#include "master/constants.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "messages/messages.hpp"

using namespace process;

using std::set;
using std::string;

namespace mesos {
namespace master {
namespace detector {

// TODO(bmahler): Consider moving these kinds of helpers into
// libprocess or a common header within mesos.
namespace promises {

// Helper for setting a set of Promises.
template <typename T>
void set(std::set<Promise<T>*>* promises, const T& t)
{
  foreach (Promise<T>* promise, *promises) {
    promise->set(t);
    delete promise;
  }
  promises->clear();
}


// Helper for failing a set of Promises.
template <typename T>
void fail(std::set<Promise<T>*>* promises, const string& failure)
{
  foreach (Promise<Option<MasterInfo>>* promise, *promises) {
    promise->fail(failure);
    delete promise;
  }
  promises->clear();
}


// Helper for discarding a set of Promises.
template <typename T>
void discard(std::set<Promise<T>*>* promises)
{
  foreach (Promise<T>* promise, *promises) {
    promise->discard();
    delete promise;
  }
  promises->clear();
}


// Helper for discarding an individual promise in the set.
template <typename T>
void discard(std::set<Promise<T>*>* promises, const Future<T>& future)
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

  Future<Option<MasterInfo>> detect(
      const Option<MasterInfo>& previous = None())
  {
    if (leader != previous) {
      return leader;
    }

    Promise<Option<MasterInfo>>* promise = new Promise<Option<MasterInfo>>();

    promise->future()
      .onDiscard(defer(self(), &Self::discard, promise->future()));

    promises.insert(promise);
    return promise->future();
  }

private:
  void discard(const Future<Option<MasterInfo>>& future)
  {
    // Discard the promise holding this future.
    promises::discard(&promises, future);
  }

  Option<MasterInfo> leader; // The appointed master.
  set<Promise<Option<MasterInfo>>*> promises;
};


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
  process = new StandaloneMasterDetectorProcess(
      mesos::internal::protobuf::createMasterInfo(leader));

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
           mesos::internal::protobuf::createMasterInfo(leader));
}


Future<Option<MasterInfo>> StandaloneMasterDetector::detect(
    const Option<MasterInfo>& previous)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::detect, previous);
}


} // namespace detector {
} // namespace master {
} // namespace mesos {
