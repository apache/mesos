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

#ifndef __MASTER_DETECTOR_ZOOKEEPER_HPP__
#define __MASTER_DETECTOR_ZOOKEEPER_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/zookeeper/group.hpp>
#include <mesos/zookeeper/url.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace master {
namespace detector {

extern const Duration MASTER_DETECTOR_ZK_SESSION_TIMEOUT;

// Forward declarations.
class ZooKeeperMasterDetectorProcess;


class ZooKeeperMasterDetector : public MasterDetector
{
public:
  // Creates a detector which uses ZooKeeper to determine (i.e.,
  // elect) a leading master.
  explicit ZooKeeperMasterDetector(
      const zookeeper::URL& url,
      const Duration& sessionTimeout = MASTER_DETECTOR_ZK_SESSION_TIMEOUT);

  // Used for testing purposes.
  explicit ZooKeeperMasterDetector(process::Owned<zookeeper::Group> group);
  ~ZooKeeperMasterDetector() override;

  // MasterDetector implementation.
  // The detector transparently tries to recover from retryable
  // errors until the group session expires, in which case the Future
  // returns None.
  process::Future<Option<MasterInfo>> detect(
      const Option<MasterInfo>& previous = None()) override;

private:
  ZooKeeperMasterDetectorProcess* process;
};

} // namespace detector {
} // namespace master {
} // namespace mesos {

#endif // __MASTER_DETECTOR_ZOOKEEPER_HPP__
