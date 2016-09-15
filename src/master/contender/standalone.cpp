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

#include "master/contender/standalone.hpp"

#include <mesos/master/contender.hpp>

#include <process/future.hpp>

using namespace process;

namespace mesos {
namespace master {
namespace contender {

StandaloneMasterContender::~StandaloneMasterContender()
{
  if (promise != nullptr) {
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


Future<Future<Nothing>> StandaloneMasterContender::contend()
{
  if (!initialized) {
    return Failure("Initialize the contender first");
  }

  if (promise != nullptr) {
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

} // namespace contender {
} // namespace master {
} // namespace mesos {
