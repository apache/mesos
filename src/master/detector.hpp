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


// An abstraction of a Master detector which can be used to
// detect the leading master from a group.
class MasterDetector
{
public:
  // Attempts to create a master detector for the specified master.
  // The master should be one of:
  //   - host:port
  //   - zk://host1:port1,host2:port2,.../path
  //   - zk://username:password@host1:port1,host2:port2,.../path
  //   - file:///path/to/file (where file contains one of the above)
  static Try<MasterDetector*> create(const std::string& master);
  virtual ~MasterDetector() = 0;

  // Returns MasterInfo after an election has occurred and the elected
  // master is different than that specified (if any), or NONE if an
  // election occurs and no master is elected (e.g., all masters are
  // lost). A failed future is returned if the detector is unable to
  // detect the leading master due to a non-retryable error.
  // Note that the detector transparently tries to recover from
  // retryable errors.
  // The future is never discarded unless it stays pending when the
  // detector destructs.
  //
  // The 'previous' result (if any) should be passed back if this
  // method is called repeatedly so the detector only returns when it
  // gets a different result.
  virtual process::Future<Option<MasterInfo> > detect(
      const Option<MasterInfo>& previous = None()) = 0;
};


// A standalone implementation of the MasterDetector with no external
// discovery mechanism so the user has to manually appoint a leader
// to the detector for it to be detected.
class StandaloneMasterDetector : public MasterDetector
{
public:
  StandaloneMasterDetector();
  // Use this constructor if the leader is known beforehand so it is
  // unnecessary to call 'appoint()' separately.
  StandaloneMasterDetector(const MasterInfo& leader);

  // Same as above but takes UPID as the parameter.
  StandaloneMasterDetector(const process::UPID& leader);

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
  ZooKeeperMasterDetector(const zookeeper::URL& url);
  // Used for testing purposes.
  ZooKeeperMasterDetector(process::Owned<zookeeper::Group> group);
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
