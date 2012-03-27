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

#include <process/process.hpp>

#include "common/try.hpp"

namespace mesos {
namespace internal {

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

} // namespace internal {
} // namespace mesos {

#endif // __MASTER_DETECTOR_HPP__

