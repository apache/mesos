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

#ifndef __CGROUPS_ISOLATION_MODULE_HPP__
#define __CGROUPS_ISOLATION_MODULE_HPP__

#include <sstream>
#include <string>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/path.hpp>

#include "launcher/launcher.hpp"

#include "slave/flags.hpp"
#include "slave/isolation_module.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"

namespace mesos {
namespace internal {
namespace slave {

class CgroupsIsolationModule
  : public IsolationModule,
    public ProcessExitedListener
{
public:
  CgroupsIsolationModule();

  virtual ~CgroupsIsolationModule();

  virtual void initialize(
      const Flags& flags,
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

  virtual void processExited(pid_t pid, int status);

private:
  // No copying, no assigning.
  CgroupsIsolationModule(const CgroupsIsolationModule&);
  CgroupsIsolationModule& operator = (const CgroupsIsolationModule&);

  // The cgroup information for each live executor.
  struct CgroupInfo
  {
    // Returns the canonicalized name of the cgroup in the filesystem.
    std::string name() const
    {
      std::ostringstream out;
      out << "framework_" << frameworkId
          << "_executor_" << executorId
          << "_tag_" << tag;
      return path::join("mesos", out.str());
    }

    FrameworkID frameworkId;
    ExecutorID executorId;

    // The UUID tag to distinguish between different launches of the same
    // executor (which have the same frameworkId and executorId).
    std::string tag;

    // PID of the forked process of the executor.
    pid_t pid;

    // Whether the executor has been killed.
    bool killed;

    // Used to cancel the OOM listening.
    process::Future<uint64_t> oomNotifier;
  };

  // The callback which will be invoked when "cpus" resource has changed.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation successes.
  Try<Nothing> cpusChanged(
      const CgroupInfo* info,
      const Resource& resource);

  // The callback which will be invoked when "mem" resource has changed.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation successes.
  Try<Nothing> memChanged(
      const CgroupInfo* info,
      const Resource& resource);

  // Start listening on OOM events. This function will create an eventfd and
  // start polling on it.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  void oomListen(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // This function is invoked when the polling on eventfd has a result.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   tag           The uuid tag.
  void oomWaited(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& tag,
      const process::Future<uint64_t>& future);

  // This function is invoked when the OOM event happens.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   tag           The uuid tag.
  void oom(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& tag);

  // This callback is invoked when destroy cgroup has a result.
  // @param   cgroup        The cgroup that is being destroyed.
  // @param   future        The future describing the destroy process.
  void destroyWaited(
      const std::string& cgroup,
      const process::Future<bool>& future);

  // Register a cgroup in the isolation module.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @return  A pointer to the cgroup info registered.
  CgroupInfo* registerCgroupInfo(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Unregister a cgroup in the isolation module.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  void unregisterCgroupInfo(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Find a registered cgroup by the PID of the leading process.
  // @param   pid           The PID of the leading process in the cgroup.
  // @return  A pointer to the cgroup info if found, NULL otherwise.
  CgroupInfo* findCgroupInfo(pid_t pid);

  // Find a registered cgroup by the frameworkId and the executorId.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @return  A pointer to the cgroup info if found, NULL otherwise.
  CgroupInfo* findCgroupInfo(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  Flags flags;
  bool local;
  process::PID<Slave> slave;
  bool initialized;
  Reaper* reaper;

  // The cgroup information for each live executor.
  hashmap<FrameworkID, hashmap<ExecutorID, CgroupInfo*> > infos;

  // The path to the cgroups hierarchy root.
  std::string hierarchy;

  // The activated cgroups subsystems that can be used by the module.
  hashset<std::string> activatedSubsystems;

  // Handlers for each resource name, used for resource changes.
  hashmap<std::string,
          Try<Nothing>(CgroupsIsolationModule::*)(
              const CgroupInfo*,
              const Resource&)> handlers;
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __CGROUPS_ISOLATION_MODULE_HPP__
