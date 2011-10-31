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

#ifndef __LXC_ISOLATION_MODULE_HPP__
#define __LXC_ISOLATION_MODULE_HPP__

#include <string>
#include <vector>

#include "isolation_module.hpp"
#include "reaper.hpp"
#include "slave.hpp"

#include "common/hashmap.hpp"


namespace mesos { namespace internal { namespace slave {

class LxcIsolationModule
  : public IsolationModule, public ProcessExitedListener
{
public:
  LxcIsolationModule();

  virtual ~LxcIsolationModule();

  virtual void initialize(const Configuration& conf,
                          bool local,
                          const process::PID<Slave>& slave);

  virtual void launchExecutor(const FrameworkID& frameworkId,
                              const FrameworkInfo& frameworkInfo,
                              const ExecutorInfo& executorInfo,
                              const std::string& directory,
                              const Resources& resources);

  virtual void killExecutor(const FrameworkID& frameworkId,
                            const ExecutorID& executorId);

  virtual void resourcesChanged(const FrameworkID& frameworkId,
                                const ExecutorID& executorId,
                                const Resources& resources);

  virtual void processExited(pid_t pid, int status);

private:
  // No copying, no assigning.
  LxcIsolationModule(const LxcIsolationModule&);
  LxcIsolationModule& operator = (const LxcIsolationModule&);

  // Attempt to set a resource limit of a container for a given
  // control group property (e.g. cpu.shares).
  bool setControlGroupValue(const std::string& container,
                            const std::string& property,
                            int64_t value);

  std::vector<std::string> getControlGroupOptions(const Resources& resources);

  // Per-framework information object maintained in info hashmap.
  struct ContainerInfo
  {
    FrameworkID frameworkId;
    ExecutorID executorId;
    std::string container; // Name of Linux container used for this framework.
    pid_t pid; // PID of lxc-execute command running the executor.
  };

  // TODO(benh): Make variables const by passing them via constructor.
  Configuration conf;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper* reaper;
  hashmap<FrameworkID, hashmap<ExecutorID, ContainerInfo*> > infos;
};

}}} // namespace mesos { namespace internal { namespace slave {

#endif // __LXC_ISOLATION_MODULE_HPP__
