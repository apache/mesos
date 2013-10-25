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
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>

#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/timer.hpp>

#include <stout/try.hpp>

#include "zookeeper/authentication.hpp"
#include "zookeeper/url.hpp"
#include "zookeeper/zookeeper.hpp"

class Watcher;
class ZooKeeper;

namespace mesos {
namespace internal {

// Returns the current master PID (waiting until a master is elected).
process::Future<process::UPID> detect(
    const std::string& master,
    bool quiet = true);


// Forward declarations.
class ZooKeeperMasterDetectorProcess;

/**
 * Implements functionality for:
 *   a) detecting masters
 *   b) contending to be a master
 */
class MasterDetector
{
public:
  virtual ~MasterDetector() = 0;

  /**
   * Creates a master detector that, given the specified master (which
   * may be a ZooKeeper URL), knows how to connect to the master, or
   * contend to be a master. The master detector sends messages to the
   * specified pid when a new master is elected, a master is lost,
   * etc.
   *
   * @param master string possibly containing zk:// or file://
   * @param pid libprocess pid to both receive our messages and be
   *   used if we should contend
   * @param contend true if should contend to be master
   * @param quite true if should limit log output
   * @return instance of MasterDetector
   */
  static Try<MasterDetector*> create(const std::string& master,
                                     const process::UPID& pid,
                                     bool contend = false,
                                     bool quiet = true);

  /**
   * Cleans up and deallocates the detector.
   */
  static void destroy(MasterDetector* detector);
};


class BasicMasterDetector : public MasterDetector
{
public:
  /**
   * Create a new master detector where the specified pid contends to
   * be the master and gets elected by default.
   *
   * @param master libprocess pid to send messages/updates and be the
   * master
   */
  BasicMasterDetector(const process::UPID& master);

  /**
   * Create a new master detector where the 'master' pid is 
   * the master (no contending).
   *
   * @param master libprocess pid to send messages/updates and be the
   * master
   * @param pid/pids libprocess pids to send messages/updates to regarding
   * the master
   * @param elect if true then contend and elect the specified master
   */
  BasicMasterDetector(const process::UPID& master,
		      const process::UPID& pid,
		      bool elect = false);

  BasicMasterDetector(const process::UPID& master,
		      const std::vector<process::UPID>& pids,
		      bool elect = false);

  virtual ~BasicMasterDetector();

private:
  const process::UPID master;
};


class ZooKeeperMasterDetector : public MasterDetector
{
public:
  /**
   * Uses ZooKeeper for both detecting masters and contending to be a
   * master.
   *
   * @param url znode path of the master
   * @param pid libprocess pid to send messages/updates to (and to
   * use for contending to be a master)
   * @param contend true if should contend to be master and false otherwise (not
   * needed for slaves and frameworks)
   * @param quiet verbosity logging level for underlying ZooKeeper library
   */
  ZooKeeperMasterDetector(const zookeeper::URL& url,
                          const process::UPID& pid,
                          bool contend,
                          bool quiet);

  virtual ~ZooKeeperMasterDetector();

  /**
   *  Returns the ZooKeeper session ID associated with this detector.
   */
  process::Future<int64_t> session();

  // Visible for testing.
  ZooKeeperMasterDetectorProcess* process;
};


// TODO(benh): Make this value configurable via flags and verify that
// it is always LESS THAN the slave heartbeat timeout.
extern const Duration ZOOKEEPER_SESSION_TIMEOUT;


class ZooKeeperMasterDetectorProcess
  : public process::Process<ZooKeeperMasterDetectorProcess>
{
public:
  ZooKeeperMasterDetectorProcess(
    const zookeeper::URL& url,
    const process::UPID& pid,
    bool contend,
    bool quiet);

  virtual ~ZooKeeperMasterDetectorProcess();

  virtual void initialize();

  // ZooKeeperMasterDetector implementation.
  int64_t session();

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const std::string& path);
  void created(const std::string& path);
  void deleted(const std::string& path);

private:
  // Handles reconnecting "timeouts" by prematurely expiring a session
  // (only used for contending instances). TODO(benh): Remove 'const
  // &' after fixing libprocess.
  void timedout(const int64_t& sessionId);

  // Attempts to detect a master.
  void detectMaster();

  const zookeeper::URL url;
  const ACL_vector acl;

  const process::UPID pid;
  bool contend;

  Watcher* watcher;
  ZooKeeper* zk;

  bool expire;
  Option<process::Timer> timer;

  std::string currentMasterSeq;
  process::UPID currentMasterPID;
};

} // namespace internal {
} // namespace mesos {

#endif // __MASTER_DETECTOR_HPP__
