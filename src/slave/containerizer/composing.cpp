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

#include <list>
#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/id.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/composing.hpp"

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace slave {


class ComposingContainerizerProcess
  : public Process<ComposingContainerizerProcess>
{
public:
  ComposingContainerizerProcess(
      const vector<Containerizer*>& containerizers)
    : ProcessBase(process::ID::generate("composing-containerizer")),
      containerizers_(containerizers) {}

  virtual ~ComposingContainerizerProcess();

  Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const map<string, string>& environment,
      bool checkpoint);

  Future<bool> launch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<ContainerInfo>& containerInfo,
      const Option<string>& user,
      const SlaveID& slaveId,
      const Option<ContainerClass>& containerClass);

  Future<http::Connection> attach(
      const ContainerID& containerId);

  Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  Future<ContainerStatus> status(
      const ContainerID& containerId);

  Future<Option<ContainerTermination>> wait(
      const ContainerID& containerId);

  Future<bool> destroy(const ContainerID& containerId);

  Future<hashset<ContainerID>> containers();

  Future<Nothing> remove(const ContainerID& containerId);

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
      const map<string, string>& environment,
      bool checkpoint,
      vector<Containerizer*>::iterator containerizer,
      bool launched);

  Future<bool> _launch(
      const ContainerID& containerId,
      bool launched);

  vector<Containerizer*> containerizers_;

  // The states that the composing containerizer cares about for the
  // container it is asked to launch.
  enum State
  {
    LAUNCHING,
    LAUNCHED,
    DESTROYING,
    // No need for DESTROYED, since we remove containers
    // once the destroy completes.
  };

  struct Container
  {
    State state;
    Containerizer* containerizer;
    Promise<bool> destroyed;
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
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
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
                  environment,
                  checkpoint);
}


Future<bool> ComposingContainerizer::launch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo,
    const Option<string>& user,
    const SlaveID& slaveId,
    const Option<ContainerClass>& containerClass)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::launch,
                  containerId,
                  commandInfo,
                  containerInfo,
                  user,
                  slaveId,
                  containerClass);
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


Future<http::Connection> ComposingContainerizer::attach(
    const ContainerID& containerId)
{
    return dispatch(process,
                    &ComposingContainerizerProcess::attach,
                    containerId);
}


Future<ResourceStatistics> ComposingContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::usage, containerId);
}


Future<ContainerStatus> ComposingContainerizer::status(
    const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::status, containerId);
}


Future<Option<ContainerTermination>> ComposingContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::wait, containerId);
}


Future<bool> ComposingContainerizer::destroy(const ContainerID& containerId)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::destroy,
                  containerId);
}


Future<hashset<ContainerID>> ComposingContainerizer::containers()
{
  return dispatch(process, &ComposingContainerizerProcess::containers);
}


Future<Nothing> ComposingContainerizer::remove(const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::remove, containerId);
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


Future<bool> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint,
    vector<Containerizer*>::iterator containerizer,
    bool launched)
{
  if (!containers_.contains(containerId)) {
    // If we are here a destroy started and finished in the interim.
    return launched;
  }

  Container* container = containers_.at(containerId);

  if (launched) {
    // Note that we don't update the state if a destroy is in progress.
    if (container->state == LAUNCHING) {
      container->state = LAUNCHED;
    }

    // Note that the return value is not impacted
    // by whether a destroy is currently in progress.
    return true;
  }

  // If we are here, the launch is not supported by `containerizer`.

  // Try the next containerizer.
  ++containerizer;

  if (containerizer == containerizers_.end()) {
    // If we are here none of the containerizers support the launch.

    // We set this to `false` because the container has no chance of
    // getting launched by any containerizer. This is similar to what
    // would happen if the destroy "started" after launch returned false.
    container->destroyed.set(false);

    // We destroy the container irrespective whether
    // a destroy is already in progress, for simplicity.
    containers_.erase(containerId);
    delete container;

    // We return false here because none of the
    // containerizers support the launch.
    return false;
  }

  if (container->state == DESTROYING) {
    // If we are here there is at least one more containerizer that could
    // potentially launch this container. But since a destroy is in progress
    // we do not try any other containerizers.

    // We set this to `true` because the destroy-in-progress stopped an
    // launch-in-progress (using the next containerizer).
    container->destroyed.set(true);

    containers_.erase(containerId);
    delete container;

    // We return failure here because there is a chance some other
    // containerizer might be able to launch this container but
    // we are not trying it because a destroy is in progress.
    return Failure("Container was destroyed while launching");
  }

  container->containerizer = *containerizer;

  return (*containerizer)->launch(
      containerId,
      taskInfo,
      executorInfo,
      directory,
      user,
      slaveId,
      environment,
      checkpoint)
    .then(defer(
        self(),
        &Self::_launch,
        containerId,
        taskInfo,
        executorInfo,
        directory,
        user,
        slaveId,
        environment,
        checkpoint,
        containerizer,
        lambda::_1));
}


Future<bool> ComposingContainerizerProcess::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Duplicate container found");
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
      environment,
      checkpoint)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                user,
                slaveId,
                environment,
                checkpoint,
                containerizer,
                lambda::_1));
}


Future<bool> ComposingContainerizerProcess::launch(
          const ContainerID& containerId,
          const CommandInfo& commandInfo,
          const Option<ContainerInfo>& containerInfo,
          const Option<string>& user,
          const SlaveID& slaveId,
          const Option<ContainerClass>& containerClass)
{
  ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  if (!containers_.contains(rootContainerId)) {
    return Failure(
        "Root container " + stringify(rootContainerId) + " not found");
  }

  // Use the containerizer that launched the root container to launch
  // the nested container.
  Containerizer* containerizer = containers_.at(rootContainerId)->containerizer;

  Container* container = new Container();
  container->state = LAUNCHING;
  container->containerizer = containerizer;
  containers_[containerId] = container;

  return containerizer->launch(
      containerId,
      commandInfo,
      containerInfo,
      user,
      slaveId,
      containerClass)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                lambda::_1));
}


Future<bool> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    bool launched)
{
  if (!containers_.contains(containerId)) {
    // If we are here a destroy started and finished in the interim.
    return launched;
  }

  Container* container = containers_.at(containerId);

  if (launched) {
    // Note that we don't update the state if a destroy is in progress.
    if (container->state == LAUNCHING) {
      container->state = LAUNCHED;
    }

    // Note that the return value is not impacted
    // by whether a destroy is currently in progress.
    return true;
  }

  // If we are here, the launch is not supported by the containerizer.

  // We set this to `false` because the container has no chance of
  // getting launched. This is similar to what would happen if the
  // destroy "started" after launch returned false.
  container->destroyed.set(false);

  // We destroy the container irrespective whether
  // a destroy is already in progress, for simplicity.
  containers_.erase(containerId);
  delete container;

  // We return false here because the launch is not supported.
  return false;
}


Future<http::Connection> ComposingContainerizerProcess::attach(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container not found");
  }

  return containers_[containerId]->containerizer->attach(containerId);
}


Future<Nothing> ComposingContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container not found");
  }

  return containers_[containerId]->containerizer->update(
      containerId, resources);
}


Future<ResourceStatistics> ComposingContainerizerProcess::usage(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container not found");
  }

  return containers_[containerId]->containerizer->usage(containerId);
}


Future<ContainerStatus> ComposingContainerizerProcess::status(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container not found");
  }

  return containers_[containerId]->containerizer->status(containerId);
}


Future<Option<ContainerTermination>> ComposingContainerizerProcess::wait(
    const ContainerID& containerId)
{
  // A nested container might have already been terminated, therefore
  // `containers_` might not contain it, but its exit status might have
  // been checkpointed.
  //
  // The containerizer that launched the root container should be able
  // to retrieve the exit status even if it has been checkpointed.
  const ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  if (!containers_.contains(rootContainerId)) {
    return None();
  }

  return containers_[rootContainerId]->containerizer->wait(containerId);
}


Future<bool> ComposingContainerizerProcess::destroy(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    // TODO(bmahler): Currently the agent does not log destroy
    // failures or unknown containers, so we log it here for now.
    // Move this logging into the callers.
    LOG(WARNING) << "Attempted to destroy unknown container " << containerId;

    return false;
  }

  Container* container = containers_.at(containerId);

  switch (container->state) {
    case DESTROYING:
      break; // No-op.

    case LAUNCHING:
      container->state = DESTROYING;

      // Forward the destroy request to the containerizer. Note that
      // a containerizer is expected to handle a destroy while
      // `launch()` is in progress. If the containerizer could not
      // handle launching the container (`launch()` returns false),
      // then the containerizer may no longer know about this
      // container. If the launch returns false, we will stop trying
      // to launch the container on other containerizers.
      container->containerizer->destroy(containerId)
        .onAny(defer(self(), [=](const Future<bool>& destroy) {
          // We defer the association of the promise in order to
          // surface a successful destroy (by setting
          // `Container.destroyed` to true in `_launch()`) when
          // the containerizer cannot handle this type of container
          // (`launch()` returns false). If we do not defer here and
          // instead associate the future right away, the setting of
          // `Container.destroy` in `_launch()` will be a no-op;
          // this might result in users waiting on the future
          // incorrectly thinking that the destroy failed when in
          // fact the destroy is implicitly successful because the
          // launch failed.
          if (containers_.contains(containerId)) {
            containers_.at(containerId)->destroyed.associate(destroy);
            delete containers_.at(containerId);
            containers_.erase(containerId);
          }
        }));

      break;

    case LAUNCHED:
      container->state = DESTROYING;

      container->destroyed.associate(
          container->containerizer->destroy(containerId));

      container->destroyed.future()
        .onAny(defer(self(), [=](const Future<bool>& destroy) {
          if (containers_.contains(containerId)) {
            delete containers_.at(containerId);
            containers_.erase(containerId);
          }
        }));

      break;
  }

  return container->destroyed.future();
}


Future<hashset<ContainerID>> ComposingContainerizerProcess::containers()
{
  return containers_.keys();
}


Future<Nothing> ComposingContainerizerProcess::remove(
    const ContainerID& containerId)
{
  // A precondition of this method is that the nested container has already
  // been terminated, hence `containers_` won't contain it. To work around it,
  // we use the containerizer that launched the root container to remove the
  // nested container.

  const ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  if (!containers_.contains(rootContainerId)) {
    return Failure(
        "Root container " + stringify(rootContainerId) + " not found");
  }

  return containers_[rootContainerId]->containerizer->remove(containerId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
