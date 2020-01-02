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

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
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

  ~ComposingContainerizerProcess() override;

  Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  Future<Containerizer::LaunchResult> launch(
      const ContainerID& containerId,
      const ContainerConfig& config,
      const map<string, string>& environment,
      const Option<std::string>& pidCheckpointPath);

  Future<http::Connection> attach(
      const ContainerID& containerId);

  Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {});

  Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  Future<ContainerStatus> status(
      const ContainerID& containerId);

  Future<Option<ContainerTermination>> wait(
      const ContainerID& containerId);

  Future<Option<ContainerTermination>> destroy(
      const ContainerID& containerId);

  Future<bool> kill(const ContainerID& containerId, int signal);

  Future<hashset<ContainerID>> containers();

  Future<Nothing> remove(const ContainerID& containerId);

  Future<Nothing> pruneImages(const vector<Image>& excludedImages);

private:
  // Continuations.
  Future<Nothing> _recover();
  Future<Nothing> __recover(
      Containerizer* containerizer,
      const hashset<ContainerID>& containers);
  static Future<Nothing> ___recover();

  Future<Containerizer::LaunchResult> _launch(
      const ContainerID& containerId,
      const ContainerConfig& config,
      const map<string, string>& environment,
      const Option<std::string>& pidCheckpointPath,
      vector<Containerizer*>::iterator containerizer,
      Containerizer::LaunchResult launchResult);

  // Continuation for nested containers.
  Future<Containerizer::LaunchResult> _launch(
      const ContainerID& containerId,
      Containerizer::LaunchResult launchResult);

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


Future<Containerizer::LaunchResult> ComposingContainerizer::launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::launch,
                  containerId,
                  containerConfig,
                  environment,
                  pidCheckpointPath);
}


Future<Nothing> ComposingContainerizer::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::update,
                  containerId,
                  resourceRequests,
                  resourceLimits);
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


Future<Option<ContainerTermination>> ComposingContainerizer::destroy(
    const ContainerID& containerId)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::destroy,
                  containerId);
}


Future<bool> ComposingContainerizer::kill(
    const ContainerID& containerId,
    int signal)
{
  return dispatch(process,
                  &ComposingContainerizerProcess::kill,
                  containerId,
                  signal);
}


Future<hashset<ContainerID>> ComposingContainerizer::containers()
{
  return dispatch(process, &ComposingContainerizerProcess::containers);
}


Future<Nothing> ComposingContainerizer::remove(const ContainerID& containerId)
{
  return dispatch(process, &ComposingContainerizerProcess::remove, containerId);
}


Future<Nothing> ComposingContainerizer::pruneImages(
    const vector<Image>& excludedImages)
{
  return dispatch(
      process, &ComposingContainerizerProcess::pruneImages, excludedImages);
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
  vector<Future<Nothing>> futures;
  foreach (Containerizer* containerizer, containerizers_) {
    futures.push_back(containerizer->recover(state));
  }

  return collect(futures)
    .then(defer(self(), &Self::_recover));
}


Future<Nothing> ComposingContainerizerProcess::_recover()
{
  // Now collect all the running containers in order to multiplex.
  vector<Future<Nothing>> futures;
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

    // This is needed for eventually removing the given container from
    // the list of active containers.
    containerizer->wait(containerId)
      .onAny(defer(self(), [=](const Future<Option<ContainerTermination>>&) {
        if (containers_.contains(containerId)) {
          delete containers_.at(containerId);
          containers_.erase(containerId);
        }
      }));
  }
  return Nothing();
}


Future<Nothing> ComposingContainerizerProcess::___recover()
{
  LOG(INFO) << "Finished recovering all containerizers";

  return Nothing();
}


Future<Containerizer::LaunchResult> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath,
    vector<Containerizer*>::iterator containerizer,
    Containerizer::LaunchResult launchResult)
{
  if (!containers_.contains(containerId)) {
    // If we are here a destroy started and finished in the interim.
    return launchResult;
  }

  Container* container = containers_.at(containerId);

  if (launchResult == Containerizer::LaunchResult::SUCCESS) {
    // Note that we don't update the state if a destroy is in progress.
    if (container->state == LAUNCHING) {
      container->state = LAUNCHED;

      // This is needed for eventually removing the given container from
      // the list of active containers.
      container->containerizer->wait(containerId)
        .onAny(defer(self(), [=](const Future<Option<ContainerTermination>>&) {
          if (containers_.contains(containerId)) {
            delete containers_.at(containerId);
            containers_.erase(containerId);
          }
        }));
    }

    // Note that the return value is not impacted
    // by whether a destroy is currently in progress.
    return Containerizer::LaunchResult::SUCCESS;
  }

  // If we are here, the launch is not supported by `containerizer`.

  // Try the next containerizer.
  ++containerizer;

  if (containerizer == containerizers_.end()) {
    // If we are here none of the containerizers support the launch.

    // We destroy the container irrespective whether
    // a destroy is already in progress, for simplicity.
    containers_.erase(containerId);
    delete container;

    // None of the containerizers support the launch.
    return Containerizer::LaunchResult::NOT_SUPPORTED;
  }

  if (container->state == DESTROYING) {
    // If we are here there is at least one more containerizer that could
    // potentially launch this container. But since a destroy is in progress
    // we do not try any other containerizers.
    //
    // We return failure here because there is a chance some other
    // containerizer might be able to launch this container but
    // we are not trying it because a destroy is in progress.
    return Failure("Container was destroyed while launching");
  }

  container->containerizer = *containerizer;

  return (*containerizer)->launch(
      containerId,
      containerConfig,
      environment,
      pidCheckpointPath)
    .then(defer(
        self(),
        &Self::_launch,
        containerId,
        containerConfig,
        environment,
        pidCheckpointPath,
        containerizer,
        lambda::_1));
}


Future<Containerizer::LaunchResult> ComposingContainerizerProcess::launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath)
{
  if (containers_.contains(containerId)) {
    return Containerizer::LaunchResult::ALREADY_LAUNCHED;
  }

  Container* container = new Container();
  container->state = LAUNCHING;
  containers_[containerId] = container;

  // For nested containers, use the containerizer that launched the
  // root container. This code path uses a different continuation
  // function because there is no need to try other containerizers.
  if (containerId.has_parent()) {
    ContainerID rootContainerId = protobuf::getRootContainerId(containerId);
    if (!containers_.contains(rootContainerId)) {
      // We do cleanup here, otherwise we cannot remove or destroy the nested
      // container due to its undefined `containerizer` field.
      containers_.erase(containerId);
      delete container;

      return Failure(
          "Root container " + stringify(rootContainerId) + " not found");
    }

    Containerizer* containerizer =
      containers_.at(rootContainerId)->containerizer;

    container->containerizer = containerizer;

    return containerizer->launch(
        containerId,
        containerConfig,
        environment,
        pidCheckpointPath)
      .then(defer(self(),
                  &Self::_launch,
                  containerId,
                  lambda::_1));
  }

  // Try each containerizer. If none of them handle the
  // TaskInfo/ExecutorInfo then return a Failure.
  vector<Containerizer*>::iterator containerizer = containerizers_.begin();
  container->containerizer = *containerizer;

  return (*containerizer)->launch(
      containerId,
      containerConfig,
      environment,
      pidCheckpointPath)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                containerConfig,
                environment,
                pidCheckpointPath,
                containerizer,
                lambda::_1));
}


Future<Containerizer::LaunchResult> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    Containerizer::LaunchResult launchResult)
{
  if (!containers_.contains(containerId)) {
    // If we are here a destroy started and finished in the interim.
    return launchResult;
  }

  Container* container = containers_.at(containerId);

  if (launchResult == Containerizer::LaunchResult::SUCCESS) {
    // Note that we don't update the state if a destroy is in progress.
    if (container->state == LAUNCHING) {
      container->state = LAUNCHED;

      // This is needed for eventually removing the given container from
      // the list of active containers.
      container->containerizer->wait(containerId)
        .onAny(defer(self(), [=](const Future<Option<ContainerTermination>>&) {
          if (containers_.contains(containerId)) {
            delete containers_.at(containerId);
            containers_.erase(containerId);
          }
        }));
    }

    // Note that the return value is not impacted
    // by whether a destroy is currently in progress.
    return Containerizer::LaunchResult::SUCCESS;
  }

  // If we are here, the launch is not supported by the containerizer.

  // We destroy the container irrespective whether
  // a destroy is already in progress, for simplicity.
  containers_.erase(containerId);
  delete container;

  return Containerizer::LaunchResult::NOT_SUPPORTED;
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
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container not found");
  }

  return containers_[containerId]->containerizer->update(
      containerId, resourceRequests, resourceLimits);
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


Future<Option<ContainerTermination>> ComposingContainerizerProcess::destroy(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    // TODO(bmahler): Currently the agent does not log destroy
    // failures or unknown containers, so we log it here for now.
    // Move this logging into the callers.
    LOG(WARNING) << "Attempted to destroy unknown container " << containerId;

    // A nested container might have already been terminated, therefore
    // `containers_` might not contain it, but its exit status might have
    // been checkpointed.
    return wait(containerId);
  }

  Container* container = containers_.at(containerId);

  if (container->state == LAUNCHING || container->state == LAUNCHED) {
    // Note that this method might be called between two successive attempts to
    // launch a container using different containerizers. In this case, we will
    // return `None`, because there is no underlying containerizer that is
    // actually aware of launching a container.
    container->state = DESTROYING;
  }

  CHECK_EQ(container->state, DESTROYING);

  return container->containerizer->destroy(containerId)
    .onAny(defer(self(), [=](const Future<Option<ContainerTermination>>&) {
      if (containers_.contains(containerId)) {
        delete containers_.at(containerId);
        containers_.erase(containerId);
      }
    }));
}


Future<bool> ComposingContainerizerProcess::kill(
    const ContainerID& containerId,
    int signal)
{
  if (!containers_.contains(containerId)) {
    return false;
  }

  return containers_.at(containerId)->containerizer->kill(containerId, signal);
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


Future<Nothing> ComposingContainerizerProcess::pruneImages(
    const vector<Image>& excludedImages)
{
  vector<Future<Nothing>> futures;

  foreach (Containerizer* containerizer, containerizers_) {
    futures.push_back(containerizer->pruneImages(excludedImages));
  }

  return collect(futures)
    .then([]() { return Nothing(); });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
