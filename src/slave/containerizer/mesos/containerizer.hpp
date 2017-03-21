// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MESOS_CONTAINERIZER_HPP__
#define __MESOS_CONTAINERIZER_HPP__

#include <list>
#include <vector>

#include <process/id.hpp>
#include <process/http.hpp>
#include <process/sequence.hpp>
#include <process/shared.hpp>

#include <process/metrics/counter.hpp>

#include <stout/hashmap.hpp>
#include <stout/multihashmap.hpp>
#include <stout/os/int_fd.hpp>

#include <mesos/slave/isolator.hpp>

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/launcher.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "slave/containerizer/mesos/isolators/gpu/nvidia.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class MesosContainerizerProcess;


class MesosContainerizer : public Containerizer
{
public:
  static Try<MesosContainerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher,
      const Option<NvidiaComponents>& nvidia = None());

  static Try<MesosContainerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher,
      const process::Owned<Launcher>& launcher,
      const process::Shared<Provisioner>& provisioner,
      const std::vector<process::Owned<mesos::slave::Isolator>>& isolators);

  virtual ~MesosContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const std::map<std::string, std::string>& environment,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<ContainerInfo>& containerInfo,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const Option<mesos::slave::ContainerClass>& containerClass = None());

  virtual process::Future<process::http::Connection> attach(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId);

  virtual process::Future<bool> destroy(
      const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

  virtual process::Future<Nothing> remove(const ContainerID& containerId);

private:
  explicit MesosContainerizer(
      const process::Owned<MesosContainerizerProcess>& process);

  process::Owned<MesosContainerizerProcess> process;
};


class MesosContainerizerProcess
  : public process::Process<MesosContainerizerProcess>
{
public:
  MesosContainerizerProcess(
      const Flags& _flags,
      Fetcher* _fetcher,
      IOSwitchboard* _ioSwitchboard,
      const process::Owned<Launcher>& _launcher,
      const process::Shared<Provisioner>& _provisioner,
      const std::vector<process::Owned<mesos::slave::Isolator>>& _isolators)
    : ProcessBase(process::ID::generate("mesos-containerizer")),
      flags(_flags),
      fetcher(_fetcher),
      ioSwitchboard(_ioSwitchboard),
      launcher(_launcher),
      provisioner(_provisioner),
      isolators(_isolators) {}

  virtual ~MesosContainerizerProcess() {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const std::map<std::string, std::string>& environment,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<ContainerInfo>& containerInfo,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const Option<mesos::slave::ContainerClass>& containerClass);

  virtual process::Future<process::http::Connection> attach(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId);

  virtual process::Future<bool> exec(
      const ContainerID& containerId,
      int_fd pipeWrite);

  virtual process::Future<bool> destroy(
      const ContainerID& containerId);

  virtual process::Future<Nothing> remove(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

private:
  enum State
  {
    PROVISIONING,
    PREPARING,
    ISOLATING,
    FETCHING,
    RUNNING,
    DESTROYING
  };

  friend std::ostream& operator<<(std::ostream& stream, const State& state);

  process::Future<Nothing> _recover(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<std::list<Nothing>> recoverIsolators(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> recoverProvisioner(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> __recover(
      const std::list<mesos::slave::ContainerState>& recovered,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const Option<ProvisionInfo>& provisionInfo);

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const SlaveID& slaveId);

  process::Future<bool> launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::map<std::string, std::string>& environment,
      const SlaveID& slaveId,
      bool checkpoint);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const Option<mesos::slave::ContainerIO>& containerIO,
      const std::map<std::string, std::string>& environment,
      const SlaveID& slaveId,
      bool checkpoint);

  process::Future<bool> isolate(
      const ContainerID& containerId,
      pid_t _pid);

  // Continues 'destroy()' once nested containers are handled.
  void _destroy(
      const ContainerID& containerId,
      const State& previousState,
      const std::list<process::Future<bool>>& destroys);

  // Continues '_destroy()' once isolators has completed.
  void __destroy(const ContainerID& containerId);

  // Continues '__destroy()' once all processes have been killed
  // by the launcher.
  void ___destroy(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  // Continues '___destroy()' once we get the exit status of the container.
  void ____destroy(const ContainerID& containerId);

  // Continues '____destroy()' once all isolators have completed
  // cleanup.
  void _____destroy(
      const ContainerID& containerId,
      const process::Future<std::list<process::Future<Nothing>>>& cleanups);

  // Continues '_____destroy()' once provisioner have completed destroy.
  void ______destroy(
      const ContainerID& containerId,
      const process::Future<bool>& destroy);

  // Call back for when an isolator limits a container and impacts the
  // processes. This will trigger container destruction.
  void limited(
      const ContainerID& containerId,
      const process::Future<mesos::slave::ContainerLimitation>& future);

  // Helper for reaping the 'init' process of a container.
  process::Future<Option<int>> reap(
      const ContainerID& containerId,
      pid_t pid);

  // Call back for when the executor exits. This will trigger container
  // destroy.
  void reaped(const ContainerID& containerId);

  // TODO(jieyu): Consider introducing an Isolators struct and moving
  // all isolator related operations to that struct.
  process::Future<std::list<process::Future<Nothing>>> cleanupIsolators(
      const ContainerID& containerId);

  const Flags flags;
  Fetcher* fetcher;
  IOSwitchboard* ioSwitchboard;
  const process::Owned<Launcher> launcher;
  const process::Shared<Provisioner> provisioner;
  const std::vector<process::Owned<mesos::slave::Isolator>> isolators;

  struct Container
  {
    Container() : sequence("mesos-container-status-updates") {}

    // Promise for futures returned from wait().
    process::Promise<mesos::slave::ContainerTermination> termination;

    // NOTE: this represents 'PID 1', i.e., the "init" of the
    // container that we created (it may be for an executor, or any
    // arbitrary process that has been launched in the event of nested
    // containers).
    Option<pid_t> pid;

    // Sandbox directory for the container. It is optional here because
    // we don't keep track of sandbox directory for orphan containers.
    Option<std::string> directory;

    // We keep track of the future exit status for the container if it
    // has been launched. If the container has not been launched yet,
    // 'status' will be set to None().
    //
    // NOTE: A container has an exit status does not mean that it has
    // been properly destroyed. We need to perform cleanup on
    // isolators and provisioner after that.
    Option<process::Future<Option<int>>> status;

    // We keep track of the future for 'provisioner->provision' so
    // that destroy will only start calling 'provisioner->destroy'
    // after 'provisioner->provision' has finished.
    process::Future<ProvisionInfo> provisioning;

    // We keep track of the future that is waiting for all the
    // 'isolator->prepare' to finish so that destroy will only start
    // calling cleanup after all isolators have finished preparing.
    process::Future<std::list<Option<mesos::slave::ContainerLaunchInfo>>>
      launchInfos;

    // We keep track of the future that is waiting for all the
    // 'isolator->isolate' futures so that destroy will only start
    // calling cleanup after all isolators have finished isolating.
    process::Future<std::list<Nothing>> isolation;

    // We keep track of any limitations received from each isolator so
    // we can determine the cause of a container termination.
    std::vector<mesos::slave::ContainerLimitation> limitations;

    // We keep track of the resources for each container so we can set
    // the ResourceStatistics limits in usage().
    Resources resources;

    // The configuration for the container to be launched. This field
    // is only used during the launch of a container.
    mesos::slave::ContainerConfig config;

    State state;

    // Used when `status` needs to be collected from isolators
    // associated with this container. `Sequence` allows us to
    // maintain the order of `status` requests for a given container.
    process::Sequence sequence;

    // Child containers nested under this container.
    hashset<ContainerID> children;
  };

  hashmap<ContainerID, process::Owned<Container>> containers_;

  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::Counter container_destroy_errors;
  } metrics;
};


std::ostream& operator<<(
    std::ostream& stream,
    const MesosContainerizerProcess::State& state);

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_HPP__
