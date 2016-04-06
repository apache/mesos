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

#ifndef __MASTER_CONTENDER_HPP__
#define __MASTER_CONTENDER_HPP__

#include <mesos/master/contender.hpp>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>

#include "messages/messages.hpp"

#include "zookeeper/contender.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

namespace mesos {
namespace internal {

extern const Duration MASTER_CONTENDER_ZK_SESSION_TIMEOUT;

class ZooKeeperMasterContenderProcess;

// A basic implementation which assumes only one master is
// contending.
class StandaloneMasterContender : public MasterContender
{
public:
  StandaloneMasterContender()
    : initialized(false),
      promise(NULL) {}

  virtual ~StandaloneMasterContender();

  // MasterContender implementation.
  virtual void initialize(const MasterInfo& masterInfo);

  // In this basic implementation the outer Future directly returns
  // and inner Future stays pending because there is only one
  // contender in the contest.
  virtual process::Future<process::Future<Nothing> > contend();

private:
  bool initialized;
  process::Promise<Nothing>* promise;
};


class ZooKeeperMasterContender : public MasterContender
{
public:
  // Creates a contender that uses ZooKeeper to determine (i.e.,
  // elect) a leading master.
  explicit ZooKeeperMasterContender(const zookeeper::URL& url);
  explicit ZooKeeperMasterContender(process::Owned<zookeeper::Group> group);

  virtual ~ZooKeeperMasterContender();

  // MasterContender implementation.
  virtual void initialize(const MasterInfo& masterInfo);
  virtual process::Future<process::Future<Nothing> > contend();

private:
  ZooKeeperMasterContenderProcess* process;
};

} // namespace internal {
} // namespace mesos {

#endif // __MASTER_CONTENDER_HPP__
