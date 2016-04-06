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

#ifndef __MASTER_DETECTOR_HPP__
#define __MASTER_DETECTOR_HPP__

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

namespace mesos {
namespace internal {

extern const Duration MASTER_DETECTOR_ZK_SESSION_TIMEOUT;

// Forward declarations.
class StandaloneMasterDetectorProcess;
class ZooKeeperMasterDetectorProcess;

// A standalone implementation of the MasterDetector with no external
// discovery mechanism so the user has to manually appoint a leader
// to the detector for it to be detected.
class StandaloneMasterDetector : public MasterDetector
{
public:
  StandaloneMasterDetector();
  // Use this constructor if the leader is known beforehand so it is
  // unnecessary to call 'appoint()' separately.
  explicit StandaloneMasterDetector(const MasterInfo& leader);

  // Same as above but takes UPID as the parameter.
  explicit StandaloneMasterDetector(const process::UPID& leader);

  virtual ~StandaloneMasterDetector();

  // Appoint the leading master so it can be *detected*.
  void appoint(const Option<MasterInfo>& leader);

  // Same as above but takes 'UPID' as the parameter.
  void appoint(const process::UPID& leader);

  virtual process::Future<Option<MasterInfo> > detect(
      const Option<MasterInfo>& previous = None());

private:
  StandaloneMasterDetectorProcess* process;
};


class ZooKeeperMasterDetector : public MasterDetector
{
public:
  // Creates a detector which uses ZooKeeper to determine (i.e.,
  // elect) a leading master.
  explicit ZooKeeperMasterDetector(const zookeeper::URL& url);
  // Used for testing purposes.
  explicit ZooKeeperMasterDetector(process::Owned<zookeeper::Group> group);
  virtual ~ZooKeeperMasterDetector();

  // MasterDetector implementation.
  // The detector transparently tries to recover from retryable
  // errors until the group session expires, in which case the Future
  // returns None.
  virtual process::Future<Option<MasterInfo> > detect(
      const Option<MasterInfo>& previous = None());

private:
  ZooKeeperMasterDetectorProcess* process;
};

} // namespace internal {
} // namespace mesos {

#endif // __MASTER_DETECTOR_HPP__
