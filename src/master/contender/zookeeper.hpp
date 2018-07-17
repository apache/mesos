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

#ifndef __MASTER_CONTENDER_ZOOKEEPER_HPP__
#define __MASTER_CONTENDER_ZOOKEEPER_HPP__

#include <mesos/mesos.hpp>

#include <mesos/master/contender.hpp>

#include <mesos/zookeeper/group.hpp>
#include <mesos/zookeeper/url.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>

namespace mesos {
namespace master {
namespace contender {

extern const Duration MASTER_CONTENDER_ZK_SESSION_TIMEOUT;


class ZooKeeperMasterContenderProcess;


class ZooKeeperMasterContender : public MasterContender
{
public:
  // Creates a contender that uses ZooKeeper to determine (i.e.,
  // elect) a leading master.
  explicit ZooKeeperMasterContender(
      const zookeeper::URL& url,
      const Duration& sessionTimeout = MASTER_CONTENDER_ZK_SESSION_TIMEOUT);

  explicit ZooKeeperMasterContender(process::Owned<zookeeper::Group> group);

  ~ZooKeeperMasterContender() override;

  // MasterContender implementation.
  void initialize(const MasterInfo& masterInfo) override;
  process::Future<process::Future<Nothing>> contend() override;

private:
  ZooKeeperMasterContenderProcess* process;
};

} // namespace contender {
} // namespace master {
} // namespace mesos {

#endif // __MASTER_CONTENDER_ZOOKEEPER_HPP__
