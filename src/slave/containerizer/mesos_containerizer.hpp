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

#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/multihashmap.hpp>

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class MesosContainerizerProcess;

class MesosContainerizer : public Containerizer
{
public:
  MesosContainerizer(
      const Flags& flags,
      bool local,
      const process::Owned<Launcher>& launcher,
      const std::vector<process::Owned<Isolator> >& isolators);

  virtual ~MesosContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
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

  virtual process::Future<Containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

private:
  MesosContainerizerProcess* process;
};


class MesosContainerizerProcess : public process::Process<MesosContainerizerProcess>
{
public:
  MesosContainerizerProcess(
      const Flags& _flags,
      bool _local,
      const process::Owned<Launcher>& _launcher,
      const std::vector<process::Owned<Isolator> >& _isolators)
    : flags(_flags),
      local(_local),
      launcher(_launcher),
      isolators(_isolators) {}

  virtual ~MesosContainerizerProcess() {}

  process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  process::Future<Nothing> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  process::Future<Containerizer::Termination> wait(
      const ContainerID& containerId);

  void destroy(const ContainerID& containerId);

private:
  process::Future<Nothing> _recover(
      const std::list<state::RunState>& recovered);

  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user);

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user);

  process::Future<pid_t> fork(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const lambda::function<int()>& inChild,
      const SlaveID& slaveId,
      bool checkpoint,
      int pipeRead);

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t _pid);

  process::Future<Nothing> _isolate(
      const ContainerID& containerId,
      const std::list<Option<CommandInfo> >& commands);

  process::Future<Nothing> exec(
      const ContainerID& containerId,
      int pipeWrite);

  // Continues 'destroy()' once all processes have been killed by the launcher.
  void _destroy(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  // Continues (and completes) '_destroy()' once we get the exit status of the
  // executor.
  void __destroy(
      const ContainerID& containerId,
      const process::Future<Option<int > >& status);

  // Call back for when an isolator limits a container and impacts the
  // processes. This will trigger container destruction.
  void limited(
      const ContainerID& containerId,
      const process::Future<Limitation>& future);

  // Call back for when the executor exits. This will trigger container
  // destroy.
  void reaped(const ContainerID& containerId);

  const Flags flags;
  const bool local;
  const process::Owned<Launcher> launcher;
  const std::vector<process::Owned<Isolator> > isolators;

  // TODO(idownes): Consider putting these per-container variables into a
  // struct.
  // Promises for futures returned from wait().
  hashmap<ContainerID,
    process::Owned<process::Promise<Containerizer::Termination> > > promises;

  // We need to keep track of the future exit status for each executor because
  // we'll only get a single notification when the executor exits.
  hashmap<ContainerID, process::Future<Option<int> > > statuses;

  // We keep track of any limitations received from each isolator so we can
  // determine the cause of an executor termination.
  multihashmap<ContainerID, Limitation> limitations;

  // We keep track of the resources for each container so we can set the
  // ResourceStatistics limits in usage().
  hashmap<ContainerID, Resources> resources;

  // Set of containers that are in process of being destroyed.
  hashset<ContainerID> destroying;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_HPP__
