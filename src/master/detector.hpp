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

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/result.hpp>
#include <stout/try.hpp>

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

  // Returns some PID after an election has occurred and the elected
  // PID is different than that specified (if any), or NONE if an
  // election occurs and no PID is elected (e.g., all PIDs are lost).
  // The result is an error if the detector is not able to detect the
  // leading master, possibly due to network disconnection.
  //
  // The future fails when the detector is destructed, it is never
  // discarded.
  //
  // The 'previous' result (if any) should be passed back if this
  // method is called repeatedly so the detector only returns when it
  // gets a different result, either because an error is recovered or
  // the elected membership is different from the 'previous'.
  virtual process::Future<Result<process::UPID> > detect(
      const Result<process::UPID>& previous = None()) = 0;
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
  StandaloneMasterDetector(const process::UPID& leader);
  virtual ~StandaloneMasterDetector();

  // Appoint the leading master so it can be *detected* by default.
  // The leader can be NONE or ERROR.
  // This method is used only by this basic implementation and not
  // needed by child classes with other detection mechanisms such as
  // Zookeeper.
  //
  // When used by Master, this method is called during its
  // initialization process; when used by Slave and SchedulerDriver,
  // the MasterDetector needs to have the leader installed prior to
  // injection.
  void appoint(const Result<process::UPID>& leader);

  virtual process::Future<Result<process::UPID> > detect(
      const Result<process::UPID>& previous = None());

private:
  StandaloneMasterDetectorProcess* process;
};


class ZooKeeperMasterDetector : public MasterDetector
{
public:
  // Creates a detector which uses ZooKeeper to determine (i.e.,
  // elect) a leading master.
  ZooKeeperMasterDetector(const zookeeper::URL& url);
  // A constructor overload for testing purposes.
  ZooKeeperMasterDetector(process::Owned<zookeeper::Group> group);
  virtual ~ZooKeeperMasterDetector();

  // MasterDetector implementation.
  virtual process::Future<Result<process::UPID> > detect(
      const Result<process::UPID>& previous = None());

private:
  ZooKeeperMasterDetectorProcess* process;
};

} // namespace internal {
} // namespace mesos {

#endif // __MASTER_DETECTOR_HPP__
