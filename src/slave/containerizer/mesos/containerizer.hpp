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

#ifndef __MESOS_CONTAINERIZER_HPP__
#define __MESOS_CONTAINERIZER_HPP__

#include <list>
#include <vector>

#include <mesos/slave/isolator.hpp>

#include <process/metrics/counter.hpp>

#include <stout/hashmap.hpp>
#include <stout/multihashmap.hpp>

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/launcher.hpp"
#include "slave/containerizer/provisioner.hpp"

namespace mesos {
namespace internal {
namespace slave {

extern const char MESOS_CONTAINERIZER[];

// Forward declaration.
class MesosContainerizerProcess;

class MesosContainerizer : public Containerizer
{
public:
  static Try<MesosContainerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher);

  MesosContainerizer(
      const Flags& flags,
      bool local,
      Fetcher* fetcher,
      const process::Owned<Launcher>& launcher,
      const std::vector<process::Owned<mesos::slave::Isolator>>& isolators,
      const hashmap<ContainerInfo::Image::Type,
                    process::Owned<Provisioner>>& provisioners);


  // Used for testing.
  MesosContainerizer(const process::Owned<MesosContainerizerProcess>& _process);

  virtual ~MesosContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

private:
  process::Owned<MesosContainerizerProcess> process;
};


class MesosContainerizerProcess
  : public process::Process<MesosContainerizerProcess>
{
public:
  MesosContainerizerProcess(
      const Flags& _flags,
      bool _local,
      Fetcher* _fetcher,
      const process::Owned<Launcher>& _launcher,
      const std::vector<process::Owned<mesos::slave::Isolator>>& _isolators,
      const hashmap<ContainerInfo::Image::Type,
                    process::Owned<Provisioner>>& _provisioners)
    : flags(_flags),
      local(_local),
      fetcher(_fetcher),
      launcher(_launcher),
      isolators(_isolators),
      provisioners(_provisioners) {}

  virtual ~MesosContainerizerProcess() {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual process::Future<bool> exec(
      const ContainerID& containerId,
      int pipeWrite);

  virtual void destroy(const ContainerID& containerId, bool killed);

  virtual process::Future<hashset<ContainerID>> containers();

  // Made public for testing.
  void ___recover(
      const ContainerID& containerId,
      const process::Future<std::list<process::Future<Nothing>>>& future);

private:
  process::Future<Nothing> _recover(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> __recover(
      const std::list<mesos::slave::ContainerState>& recovered,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> provision(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const SlaveID& slaveId,
      const std::string& directory,
      bool checkpoint);

  process::Future<Nothing> _provision(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const SlaveID& slaveId,
      bool checkpoint,
      const std::string& rootfs);

  process::Future<std::list<Option<mesos::slave::ContainerPrepareInfo>>>
    prepare(const ContainerID& containerId,
            const ExecutorInfo& executorInfo,
            const std::string& directory,
            const Option<std::string>& user);

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint,
      const std::list<Option<mesos::slave::ContainerPrepareInfo>>& scripts);

  process::Future<bool> isolate(
      const ContainerID& containerId,
      pid_t _pid);

  // Continues 'destroy()' once isolators has completed.
  void _destroy(const ContainerID& containerId, bool killed);

  // Continues 'destroy()' once all processes have been killed by the launcher.
  void __destroy(
      const ContainerID& containerId,
      const process::Future<Nothing>& future,
      bool killed);

  // Continues '_destroy()' once we get the exit status of the executor.
  void ___destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      const Option<std::string>& message,
      bool killed);

  // Continues '__destroy()' once all isolators have completed
  // cleanup.
  void ____destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      const process::Future<std::list<process::Future<Nothing>>>& cleanups,
      Option<std::string> message,
      bool killed);

  // Continues (and completes) '__destroy()' once any root filessystem
  // has been cleaned up.
  void _____destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      Option<std::string> message,
      bool killed,
      const process::Future<Nothing>& cleanup);

  // Call back for when an isolator limits a container and impacts the
  // processes. This will trigger container destruction.
  void limited(
      const ContainerID& containerId,
      const process::Future<mesos::slave::ContainerLimitation>& future);

  // Call back for when the executor exits. This will trigger container
  // destroy.
  void reaped(const ContainerID& containerId);

  // TODO(jieyu): Consider introducing an Isolators struct and moving
  // all isolator related operations to that struct.
  process::Future<std::list<process::Future<Nothing>>> cleanupIsolators(
      const ContainerID& containerId);

  const Flags flags;
  const bool local;
  Fetcher* fetcher;
  const process::Owned<Launcher> launcher;
  const std::vector<process::Owned<mesos::slave::Isolator>> isolators;
  hashmap<ContainerInfo::Image::Type, process::Owned<Provisioner>> provisioners;

  enum State
  {
    PREPARING,
    ISOLATING,
    FETCHING,
    RUNNING,
    DESTROYING
  };

  struct Container
  {
    // Promise for futures returned from wait().
    process::Promise<containerizer::Termination> promise;

    // We need to keep track of the future exit status for each
    // executor because we'll only get a single notification when
    // the executor exits.
    process::Future<Option<int>> status;

    // We keep track of the future that is waiting for all the
    // isolators' prepare futures, so that destroy will only start
    // calling cleanup after all isolators has finished preparing.
    process::Future<std::list<Option<mesos::slave::ContainerPrepareInfo>>>
      prepareInfos;

    // We keep track of the future that is waiting for all the
    // isolators' isolate futures, so that destroy will only start
    // calling cleanup after all isolators has finished isolating.
    process::Future<std::list<Nothing>> isolation;

    // We keep track of any limitations received from each isolator so we can
    // determine the cause of an executor termination.
    std::vector<mesos::slave::ContainerLimitation> limitations;

    // We keep track of the resources for each container so we can set the
    // ResourceStatistics limits in usage().
    Resources resources;

    // We keep track of the ExecutorInfo so we have the optional ContainerInfo.
    ExecutorInfo executorInfo;

    // The executor's working directory on the host.
    std::string directory;

    // The path to the container's rootfs, if full filesystem
    // isolation is used.
    Option<std::string> rootfs;

    State state;
  };

  hashmap<ContainerID, process::Owned<Container>> containers_;

  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::Counter container_destroy_errors;
  } metrics;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_HPP__
