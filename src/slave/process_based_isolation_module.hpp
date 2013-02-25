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

#ifndef __PROCESS_BASED_ISOLATION_MODULE_HPP__
#define __PROCESS_BASED_ISOLATION_MODULE_HPP__

#include <string>

#include <sys/types.h>

#include <process/future.hpp>

#include <stout/hashmap.hpp>

#include "launcher/launcher.hpp"

#include "slave/flags.hpp"
#include "slave/isolation_module.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"

namespace mesos {
namespace internal {
namespace slave {

class ProcessBasedIsolationModule
  : public IsolationModule, public ProcessExitedListener
{
public:
  ProcessBasedIsolationModule();

  virtual ~ProcessBasedIsolationModule();

  virtual void initialize(
      const Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<Slave>& slave);

  virtual void launchExecutor(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
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

  virtual void processExited(pid_t pid, int status);

protected:
  // Main method executed after a fork() to create a Launcher for launching
  // an executor's process. The Launcher will chdir() to the child's working
  // directory, fetch the executor, set environment varibles,
  // switch user, etc, and finally exec() the executor process.
  // Subclasses of ProcessBasedIsolationModule that wish to override the
  // default launching behavior should override createLauncher() and return
  // their own Launcher object (including possibly a subclass of Launcher).
  virtual launcher::ExecutorLauncher* createExecutorLauncher(
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory);

private:
  // No copying, no assigning.
  ProcessBasedIsolationModule(const ProcessBasedIsolationModule&);
  ProcessBasedIsolationModule& operator = (const ProcessBasedIsolationModule&);

  struct ProcessInfo
  {
    FrameworkID frameworkId;
    ExecutorID executorId;
    pid_t pid; // PID of the forked executor process.
    std::string directory; // Working directory of the executor.
  };

  // TODO(benh): Make variables const by passing them via constructor.
  Flags flags;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper* reaper;
  hashmap<FrameworkID, hashmap<ExecutorID, ProcessInfo*> > infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROCESS_BASED_ISOLATION_MODULE_HPP__
