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

#include <list>
#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/composing.hpp"

using std::list;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {


class ComposingContainerizerProcess
  : public Process<ComposingContainerizerProcess>
{
public:
  ComposingContainerizerProcess(
      const vector<Containerizer*>& containerizers)
    : containerizers_(containerizers) {}

  virtual ~ComposingContainerizerProcess();

  Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  void destroy(const ContainerID& containerId);

  Future<hashset<ContainerID>> containers();

private:
  // Continuations.
  Future<Nothing> _recover();
  Future<Nothing> __recover(
      Containerizer* containerizer,
      const hashset<ContainerID>& containers);
  static Future<Nothing> ___recover();

  Future<bool> _launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint,
      vector<Containerizer*>::iterator containerizer,
      bool launched);

  vector<Containerizer*> containerizers_;

  // The states that the composing containerizer cares about for the
  // container it is asked to launch.
  enum State {
    LAUNCHING,
    LAUNCHED,
    DESTROYED
  };

  struct Container
  {
    State state;
    Containerizer* containerizer;
  };

  hashmap<ContainerID, Container*> containers_;
};


Try<ComposingContainerizer*> ComposingContainerizer::create(
    const vector<Containerizer*>& containerizers)
{
  return new ComposingContainerizer(containerizers);
}


ComposingContainerizer::ComposingContainerizer(
    const vector<Containerizer*>& containerizers)
{
  process = new ComposingContainerizerProcess(containerizers);
  spawn(process);
}


ComposingContainerizer::~ComposingContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> ComposingContainerizer::recover(
    const Option<state::SlaveState>& state)
{
  return dispatch(process, &ComposingContainerizerProcess::recover, state);
}


Future<bool> ComposingContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::launch,
                  containerId,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<bool> ComposingContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::launch,
                  containerId,
                  taskInfo,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<Nothing> ComposingContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> ComposingContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::usage, containerId);
}


Future<containerizer::Termination> ComposingContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::wait, containerId);
}


void ComposingContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &ComposingContainerizerProcess::destroy, containerId);
}


Future<hashset<ContainerID>> ComposingContainerizer::containers()
{
  return dispatch(process, &ComposingContainerizerProcess::containers);
}


ComposingContainerizerProcess::~ComposingContainerizerProcess()
{
  foreach (Containerizer* containerizer, containerizers_) {
    delete containerizer;
  }

  foreachvalue (Container* container, containers_) {
    delete container;
  }

  containerizers_.clear();
  containers_.clear();
}


Future<Nothing> ComposingContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  // Recover each containerizer in parallel.
  list<Future<Nothing>> futures;
  foreach (Containerizer* containerizer, containerizers_) {
    futures.push_back(containerizer->recover(state));
  }

  return collect(futures)
    .then(defer(self(), &Self::_recover));
}


Future<Nothing> ComposingContainerizerProcess::_recover()
{
  // Now collect all the running containers in order to multiplex.
  list<Future<Nothing>> futures;
  foreach (Containerizer* containerizer, containerizers_) {
    Future<Nothing> future = containerizer->containers()
      .then(defer(self(), &Self::__recover, containerizer, lambda::_1));
    futures.push_back(future);
  }

  return collect(futures)
    .then(lambda::bind(&Self::___recover));
}


Future<Nothing> ComposingContainerizerProcess::__recover(
    Containerizer* containerizer,
    const hashset<ContainerID>& containers)
{
  foreach (const ContainerID& containerId, containers) {
    Container* container = new Container();
    container->state = LAUNCHED;
    container->containerizer = containerizer;
    containers_[containerId] = container;
  }
  return Nothing();
}


Future<Nothing> ComposingContainerizerProcess::___recover()
{
  return Nothing();
}


Future<bool> ComposingContainerizerProcess::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Container '" + containerId.value() +
                   "' is already launching");
  }

  // Try each containerizer. If none of them handle the
  // TaskInfo/ExecutorInfo then return a Failure.
  vector<Containerizer*>::iterator containerizer = containerizers_.begin();

  Container* container = new Container();
  container->state = LAUNCHING;
  container->containerizer = *containerizer;
  containers_[containerId] = container;

  return (*containerizer)->launch(
      containerId,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                None(),
                executorInfo,
                directory,
                user,
                slaveId,
                slavePid,
                checkpoint,
                containerizer,
                lambda::_1));
}


Future<bool> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    vector<Containerizer*>::iterator containerizer,
    bool launched)
{
  // The container struct won't be cleaned up by destroy because
  // in destroy we only forward the destroy, and wait until the
  // launch returns and clean up here.
  CHECK(containers_.contains(containerId));
  Container* container = containers_[containerId];
  if (container->state == DESTROYED) {
    containers_.erase(containerId);
    delete container;
    return Failure("Container was destroyed while launching");
  }

  if (launched) {
    container->state = LAUNCHED;
    return true;
  }

  // Try the next containerizer.
  ++containerizer;

  if (containerizer == containerizers_.end()) {
    containers_.erase(containerId);
    delete container;
    return false;
  }

  container->containerizer = *containerizer;

  Future<bool> f = taskInfo.isSome() ?
      (*containerizer)->launch(
          containerId,
          taskInfo.get(),
          executorInfo,
          directory,
          user,
          slaveId,
          slavePid,
          checkpoint) :
      (*containerizer)->launch(
          containerId,
          executorInfo,
          directory,
          user,
          slaveId,
          slavePid,
          checkpoint);

  return f.then(
      defer(self(),
            &Self::_launch,
            containerId,
            taskInfo,
            executorInfo,
            directory,
            user,
            slaveId,
            slavePid,
            checkpoint,
            containerizer,
            lambda::_1));
}


Future<bool> ComposingContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Container '" + stringify(containerId) +
                   "' is already launching");
  }

  // Try each containerizer. If none of them handle the
  // TaskInfo/ExecutorInfo then return a Failure.
  vector<Containerizer*>::iterator containerizer = containerizers_.begin();

  Container* container = new Container();
  container->state = LAUNCHING;
  container->containerizer = *containerizer;
  containers_[containerId] = container;

  return (*containerizer)->launch(
      containerId,
      taskInfo,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                user,
                slaveId,
                slavePid,
                checkpoint,
                containerizer,
                lambda::_1));
}


Future<Nothing> ComposingContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not found");
  }

  return containers_[containerId]->containerizer->update(
      containerId, resources);
}


Future<ResourceStatistics> ComposingContainerizerProcess::usage(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not found");
  }

  return containers_[containerId]->containerizer->usage(containerId);
}


Future<containerizer::Termination> ComposingContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container '" + containerId.value() + "' not found");
  }

  return containers_[containerId]->containerizer->wait(containerId);
}


void ComposingContainerizerProcess::destroy(const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Container '" << containerId.value() << "' not found";
    return;
  }

  Container* container = containers_[containerId];

  if (container->state == DESTROYED) {
    LOG(WARNING) << "Container '" << containerId.value()
                 << "' is already destroyed";
    return;
  }

  // It's ok to forward destroy to any containerizer which is currently
  // launching the container, because we expect each containerizer to
  // handle calling destroy on non-existing container.
  // The composing containerizer will not move to the next
  // containerizer for a container that is destroyed as well.
  container->containerizer->destroy(containerId);

  if (container->state == LAUNCHING) {
    // Record the fact that this container was asked to be destroyed
    // so that we won't try and launch this container using any other
    // containerizers in the event the current containerizer has
    // decided it can't launch the container.
    container->state = DESTROYED;
    return;
  }

  // If the container is launched, then we can simply cleanup.
  containers_.erase(containerId);
  delete container;
}


Future<hashset<ContainerID>> ComposingContainerizerProcess::containers()
{
  return containers_.keys();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
