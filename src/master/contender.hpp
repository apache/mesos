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

#ifndef __MASTER_CONTENDER_HPP__
#define __MASTER_CONTENDER_HPP__

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


// Forward declarations.
namespace master {
class Master;
}


class ZooKeeperMasterContenderProcess;


// An abstraction for contending to be a leading master.
class MasterContender
{
public:
  // Attempts to create a master contender using the specified
  // mechanism.
  // The mechanism address should be one of:
  //   - zk://host1:port1,host2:port2,.../path
  //   - zk://username:password@host1:port1,host2:port2,.../path
  // Note that the returned contender still needs to be 'initialize()'d.
  static Try<MasterContender*> create(const std::string& mechanism);

  // Note that the contender's membership, if obtained, is scheduled
  // to be cancelled during destruction.
  virtual ~MasterContender() = 0;

  // Initializes the contender with the MasterInfo of the master it
  // contends on behalf of.
  virtual void initialize(const MasterInfo& masterInfo) = 0;

  // Returns a Future<Nothing> once the contender has entered the
  // contest (by obtaining a membership) and an error otherwise.
  // A failed future is returned if this method is called before
  // initialize().
  // The inner Future returns Nothing when the contender is out of
  // the contest (i.e. its membership is lost).
  //
  // This method can be used to contend again after candidacy is
  // obtained (the outer future satisfied), otherwise the future for
  // the pending election is returned.
  // Recontending after candidacy is obtained causes the previous
  // candidacy to be withdrawn.
  virtual process::Future<process::Future<Nothing> > contend() = 0;
};


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
