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

#ifndef __CGROUPS_ISOLATOR_HPP__
#define __CGROUPS_ISOLATOR_HPP__

#include <unistd.h>

#include <map>
#include <sstream>
#include <string>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/uuid.hpp>

#include "launcher/launcher.hpp"

#include "slave/flags.hpp"
#include "slave/isolator.hpp"
#include "slave/slave.hpp"

namespace mesos {
namespace internal {
namespace slave {

// TODO(bmahler): Migrate this into it's own file, along with moving
// all cgroups code inside of a 'cgroups' directory.
class Cpuset
{
public:
  // Grows this cpu set by the provided delta.
  // @param   delta   Amount of cpus to grow by.
  // @param   usage   Cpu usage, as allocated by the cgroups isolator.
  // @return  The new cpu allocations made by this Cpuset.
  std::map<proc::CPU, double> grow(
      double delta,
      const std::map<proc::CPU, double>& usage);

  // Shrinks this cpu set by the provided delta.
  // @param   delta   Amount of cpus to shrink by.
  // @return  The new cpu deallocations made by this Cpuset.
  std::map<proc::CPU, double> shrink(double delta);

  // @return The total cpu usage across all the cpus in this Cpuset.
  double usage() const;

  friend std::ostream& operator << (std::ostream& out, const Cpuset& cpuset);

private:
  std::map<proc::CPU, double> cpus; // CPU id -> % allocated.
};


class CgroupsIsolator : public Isolator
{
public:
  CgroupsIsolator();

  virtual void initialize(
      const Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<Slave>& slave);

  virtual void finalize();

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

private:
  // No copying, no assigning.
  CgroupsIsolator(const CgroupsIsolator&);
  CgroupsIsolator& operator = (const CgroupsIsolator&);

  void reaped(pid_t pid, const Future<Option<int> >& status);

  // The cgroup information for each live executor.
  struct CgroupInfo
  {
    ~CgroupInfo()
    {
      if (cpuset != NULL) {
        delete cpuset;
        cpuset = NULL;
      }
    }

    // Returns the canonicalized name of the cgroup in the filesystem.
    std::string name() const
    {
      CHECK_SOME(uuid);
      std::ostringstream out;
      out << "framework_" << frameworkId
          << "_executor_" << executorId
          << "_tag_" << uuid.get();
      return path::join(flags.cgroups_root, out.str());
    }

    FrameworkID frameworkId;
    ExecutorID executorId;

    // The UUID to distinguish between different launches of the same
    // executor (which have the same frameworkId and executorId).
    Option<UUID> uuid;

    // PID of the forked process of the executor.
    Option<pid_t> pid;

    bool killed; // True if "killing" has been initiated via 'killExecutor()'.

    // Indicates if this executor has been destroyed by the isolator.
    // NOTE: An executor may have terminated due to reasons
    // other than destruction by the isolator (e.g. killed by
    // slave, exited, etc.).
    bool destroyed;

    std::string message; // The reason behind the destruction.

    Option<int> status; // Exit status of the executor.

    Flags flags; // Slave flags.

    Resources resources; // Resources allocated to the cgroup.

    // Used to cancel the OOM listening.
    process::Future<uint64_t> oomNotifier;

    // CPUs allocated if using 'cpuset' subsystem.
    Cpuset* cpuset;
  };

  // The callback which will be invoked when "cpus" resource has changed.
  // @param   info          The Cgroup information.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation succeeds.
  Try<Nothing> cpusChanged(
      CgroupInfo* info,
      const Resource& resource);

  // The callback which will be invoked when "cpus" resource has changed.
  // This is only invoked when we are using the cpuset subsystem.
  // @param   info          The Cgroup information.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation succeeds.
  Try<Nothing> cpusetChanged(
      CgroupInfo* info,
      const Resource& resource);

  // The callback which will be invoked when "cpus" resource has changed,
  // and the cfs cgroups feature flag is enabled..
  // @param   info          The Cgroup information.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation succeeds.
  Try<Nothing> cfsChanged(
      CgroupInfo* info,
      const Resource& resource);

  // The callback which will be invoked when "mem" resource has changed.
  // @param   info          The Cgroup information.
  // @param   resources     The handle for the resources.
  // @return  Whether the operation succeeds.
  Try<Nothing> memChanged(
      CgroupInfo* info,
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
  // @param   uuid          The uuid of the given executor.
  void oomWaited(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid,
      const process::Future<uint64_t>& future);

  // This function is invoked when the OOM event happens.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   uuid          The uuid of the given executor.
  void oom(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid);

  // This callback is invoked when destroy cgroup has a result.
  // @param   info        The information of cgroup that is being destroyed.
  // @param   future      The future describing the destroy process.
  void _killExecutor(
      CgroupInfo* info,
      const process::Future<bool>& future);

  // This callback is invoked when destroying orphaned cgroups from the
  // previous slave execution.
  // @param   cgroup        The cgroup that is being destroyed.
  // @param   future        The future describing the destroy process.
  void _destroy(
      const std::string& cgroup,
      const process::Future<bool>& future);

  // Register a cgroup in the isolator.
  // @param   frameworkId   The id of the given framework.
  // @param   executorId    The id of the given executor.
  // @param   uuid          The uuid of the given executor run.
  // @param   pid           The executor pid.
  // @param   flags         The slave flags.
  // @return  A pointer to the cgroup info registered.
  CgroupInfo* registerCgroupInfo(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid,
      const Option<pid_t>& pid,
      const Flags& flags);

  // Unregister a cgroup in the isolator.
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

  // File descriptor to 'mesos/tasks' file in the cgroup on which we place
  // an advisory lock.
  Option<int> lockFile;

  // The cgroup information for each live executor.
  hashmap<FrameworkID, hashmap<ExecutorID, CgroupInfo*> > infos;

  // The path to the cgroups hierarchy root.
  std::string hierarchy;

  // The cgroups subsystems being used.
  hashset<std::string> subsystems;

  // Allocated cpus (if using cpuset subsystem).
  std::map<proc::CPU, double> cpus;

  // Handlers for each resource name, used for resource changes.
  hashmap<std::string,
          Try<Nothing>(CgroupsIsolator::*)(
              CgroupInfo*,
              const Resource&)> handlers;
};

} // namespace mesos {
} // namespace internal {
} // namespace slave {

#endif // __CGROUPS_ISOLATOR_HPP__
