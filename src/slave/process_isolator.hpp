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

#ifndef __PROCESS_ISOLATOR_HPP__
#define __PROCESS_ISOLATOR_HPP__

#include <string>

#include <sys/types.h>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "launcher/launcher.hpp"

#include "slave/flags.hpp"
#include "slave/isolator.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"

namespace mesos {
namespace internal {
namespace slave {

class ProcessIsolator : public Isolator, public ProcessExitedListener
{
public:
  ProcessIsolator();

  virtual void initialize(
      const Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<Slave>& slave);

  virtual void launchExecutor(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const UUID& uuid,
      const std::string& directory,
      const Resources& resources);

  virtual void killExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  virtual void resourcesChanged(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual void processExited(pid_t pid, int status);

private:
  // No copying, no assigning.
  ProcessIsolator(const ProcessIsolator&);
  ProcessIsolator& operator = (const ProcessIsolator&);

  struct ProcessInfo
  {
    ProcessInfo(const FrameworkID& _frameworkId,
                const ExecutorID& _executorId,
                const Option<pid_t>& _pid = None(),
                bool _killed = false)
      : frameworkId(_frameworkId),
        executorId(_executorId),
        pid(_pid),
        killed(_killed) {}

    FrameworkID frameworkId;
    ExecutorID executorId;
    Option<pid_t> pid; // PID of the forked executor process.
    bool killed; // True if "killing" has been initiated via 'killExecutor'.
    Resources resources; // Resources allocated to the process tree.
  };

  // TODO(benh): Make variables const by passing them via constructor.
  Flags flags;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper reaper;
  hashmap<FrameworkID, hashmap<ExecutorID, ProcessInfo*> > infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROCESS_ISOLATOR_HPP__
