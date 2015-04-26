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

#ifndef __EXTERNAL_CONTAINERIZER_HPP__
#define __EXTERNAL_CONTAINERIZER_HPP__

#include <list>
#include <sstream>
#include <string>
#include <tuple>

#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/launcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The scheme an external containerizer programs have to adhere to is;
//
// COMMAND < INPUT-PROTO > RESULT-PROTO
//
// launch < containerizer::Launch
// update < containerizer::Update
// usage < containerizer::Usage > mesos::ResourceStatistics
// wait < containerizer::Wait > containerizer::Termination
// destroy < containerizer::Destroy
// containers > containerizer::Containers
// recover
//
// 'wait' on the external containerizer side is expected to block
// until the task command/executor has terminated.
//
// Additionally, we have the following environment variable setup
// for external containerizer programs:
// MESOS_LIBEXEC_DIRECTORY = path to mesos-executor, mesos-usage, ...
// MESOS_WORK_DIRECTORY = slave work directory. This should be used
// for distiguishing slave instances.
// MESOS_DEFAULT_CONTAINER_IMAGE = default image as provided via
// slave flags (default_container_image). This variable is provided
// only in calls to 'launch'.

// Check src/examples/python/test_containerizer.py for a rough
// implementation template of this protocol.

// For debugging purposes of an external containerizer program, it
// might be helpful to enable verbose logging on the slave (GLOG_v=2).

class ExternalContainerizerProcess;

class ExternalContainerizer : public Containerizer
{
public:
  static Try<ExternalContainerizer*> create(const Flags& flags);

  ExternalContainerizer(const Flags& flags);

  virtual ~ExternalContainerizer();

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
      const TaskInfo& task,
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
  process::Owned<ExternalContainerizerProcess> process;
};


class ExternalContainerizerProcess
  : public process::Process<ExternalContainerizerProcess>
{
public:
  ExternalContainerizerProcess(const Flags& flags);

  // Recover containerized executors as specified by state. See
  // containerizer.hpp:recover for more.
  process::Future<Nothing> recover(const Option<state::SlaveState>& state);

  // Start the containerized executor.
  process::Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  // Update the container's resources.
  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  // Gather resource usage statistics for the containerized executor.
  process::Future<ResourceStatistics> usage(const ContainerID& containerId);

  // Get a future on the containerized executor's Termination.
  process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  // Terminate the containerized executor.
  void destroy(const ContainerID& containerId);

  // Get all active container-id's.
  process::Future<hashset<ContainerID>> containers();

private:
  // Startup flags.
  const Flags flags;

  // Information describing a container environment. A sandbox has to
  // be prepared before the external containerizer can be invoked.
  struct Sandbox
  {
    Sandbox(const std::string& directory, const Option<std::string>& user)
      : directory(directory), user(user) {}

    const std::string directory;
    const Option<std::string> user;
  };

  // Information describing a running container.
  struct Container
  {
    Container(const Option<Sandbox>& sandbox)
      : sandbox(sandbox), pid(None()), destroying(false) {}

    // Keep sandbox information available for subsequent containerizer
    // invocations.
    Option<Sandbox> sandbox;

    // External containerizer pid as per wait-invocation.
    // Wait should block on the external containerizer side, hence we
    // need to keep its pid for terminating if needed.
    Option<pid_t> pid;

    process::Promise<containerizer::Termination> termination;

    // Is set when container is being destroyed.
    bool destroying;

    // As described in MESOS-1251, we need to make sure that events
    // that are triggered before launch has completed, are in fact
    // queued until then to reduce complexity within external
    // containerizer program implementations. To achieve that, we
    // simply queue all events onto this promise.
    // TODO(tillt): Consider adding a timeout when queuing onto this
    // promise to account for external containerizer launch
    // invocations that got stuck.
    process::Promise<Nothing> launched;

    Resources resources;
  };

  // Stores all active containers.
  hashmap<ContainerID, process::Owned<Container>> actives;

  process::Future<Nothing> _recover(
      const Option<state::SlaveState>& state,
      const process::Future<Option<int>>& future);

  process::Future<Nothing> __recover(
      const Option<state::SlaveState>& state,
      const hashset<ContainerID>& containers);

  process::Future<Nothing> ___recover();

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const process::Future<Option<int>>& future);

  void __launch(
      const ContainerID& containerId,
      const process::Future<bool>& future);

  process::Future<containerizer::Termination> _wait(
      const ContainerID& containerId);

  void __wait(
      const ContainerID& containerId,
      const process::Future<std::tuple<
          process::Future<Result<containerizer::Termination>>,
          process::Future<Option<int>>>>& future);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources);

  process::Future<Nothing> __update(
      const ContainerID& containerId,
      const process::Future<Option<int>>& future);

  process::Future<ResourceStatistics> _usage(
      const ContainerID& containerId);

  process::Future<ResourceStatistics> __usage(
      const ContainerID& containerId,
      const process::Future<std::tuple<
          process::Future<Result<ResourceStatistics>>,
          process::Future<Option<int>>>>& future);

  void _destroy(const ContainerID& containerId);

  void __destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& future);

  process::Future<hashset<ContainerID>> _containers(
      const process::Future<std::tuple<
          process::Future<Result<containerizer::Containers>>,
          process::Future<Option<int>>>>& future);

  // Abort a possibly pending "wait" in the external containerizer
  // process.
  void unwait(const ContainerID& containerId);

  // Call back for when the containerizer has terminated all processes
  // in the container.
  void cleanup(const ContainerID& containerId);

  // Invoke the external containerizer with the given command.
  Try<process::Subprocess> invoke(
      const std::string& command,
      const Option<Sandbox>& sandbox = None(),
      const Option<std::map<std::string, std::string>>& environment = None());

  // Invoke the external containerizer with the given command and
  // a protobuf message to be piped into its stdin.
  // There can not be an Option<google::protobuf::Message> due to the
  // pure virtual members of that class, hence this override is
  // needed.
  Try<process::Subprocess> invoke(
      const std::string& command,
      const google::protobuf::Message& message,
      const Option<Sandbox>& sandbox = None(),
      const Option<std::map<std::string, std::string>>& environment = None());
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __EXTERNAL_CONTAINERIZER_HPP__
