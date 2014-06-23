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
      const vector<Containerizer*>& containerizers);

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

  Future<hashset<ContainerID> > containers();

private:
  // Continuations.
  Future<Nothing> _recover();
  Future<Nothing> __recover(
      Containerizer* containerizer,
      const hashset<ContainerID>& containers);
  static Future<Nothing> ___recover();

  Future<bool> _launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint,
      vector<Containerizer*>::iterator containerizer,
      bool launched);

  Future<bool> _launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint,
      vector<Containerizer*>::iterator containerizer,
      bool launched);

  vector<Containerizer*> containerizers_;
  hashmap<Containerizer*, hashset<ContainerID> > containers_;
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


Future<hashset<ContainerID> > ComposingContainerizer::containers()
{
  return dispatch(process, &ComposingContainerizerProcess::containers);
}


ComposingContainerizerProcess::ComposingContainerizerProcess(
    const vector<Containerizer*>& containerizers)
  : containerizers_(containerizers)
{
  foreach (Containerizer* containerizer, containerizers_) {
    containers_[containerizer] = hashset<ContainerID>();
  }
}


ComposingContainerizerProcess::~ComposingContainerizerProcess()
{
  foreach (Containerizer* containerizer, containerizers_) {
    delete containerizer;
  }
  containerizers_.clear();
  containers_.clear();
}


Future<Nothing> ComposingContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  // Recover each containerizer in parallel.
  list<Future<Nothing> > futures;
  foreach (Containerizer* containerizer, containerizers_) {
    futures.push_back(containerizer->recover(state));
  }

  return collect(futures)
    .then(defer(self(), &Self::_recover));
}


Future<Nothing> ComposingContainerizerProcess::_recover()
{
  // Now collect all the running containers in order to multiplex.
  list<Future<Nothing> > futures;
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
  containers_[containerizer] = containers;
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
  // Try each containerizer. If none of them handle the
  // TaskInfo/ExecutorInfo then return a Failure.
  vector<Containerizer*>::iterator containerizer = containerizers_.begin();

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
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    vector<Containerizer*>::iterator containerizer,
    bool launched)
{
  if (launched) {
    containers_[*containerizer].insert(containerId);
    return true;
  }

  // Try the next containerizer.
  ++containerizer;

  if (containerizer == containerizers_.end()) {
    return false;
  }

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
  // Try each containerizer. If none of them handle the
  // TaskInfo/ExecutorInfo then return a Failure.
  vector<Containerizer*>::iterator containerizer = containerizers_.begin();

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

Future<bool> ComposingContainerizerProcess::_launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    vector<Containerizer*>::iterator containerizer,
    bool launched)
{
  if (launched) {
    containers_[*containerizer].insert(containerId);
    return true;
  }

  // Try the next containerizer.
  ++containerizer;

  if (containerizer == containerizers_.end()) {
    return false;
  }

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
  foreachpair (Containerizer* containerizer,
               const hashset<ContainerID>& containers,
               containers_) {
    if (containers.contains(containerId)) {
      return containerizer->update(containerId, resources);
    }
  }

  return Failure("No container found");
}


Future<ResourceStatistics> ComposingContainerizerProcess::usage(
    const ContainerID& containerId)
{
  foreachpair (Containerizer* containerizer,
               const hashset<ContainerID>& containers,
               containers_) {
    if (containers.contains(containerId)) {
      return containerizer->usage(containerId);
    }
  }

  return Failure("No container found");
}


Future<containerizer::Termination> ComposingContainerizerProcess::wait(
    const ContainerID& containerId)
{
  foreachpair (Containerizer* containerizer,
               const hashset<ContainerID>& containers,
               containers_) {
    if (containers.contains(containerId)) {
      return containerizer->wait(containerId);
    }
  }

  return Failure("No container found");
}


void ComposingContainerizerProcess::destroy(const ContainerID& containerId)
{
  foreachpair (Containerizer* containerizer,
               const hashset<ContainerID>& containers,
               containers_) {
    if (containers.contains(containerId)) {
      containerizer->destroy(containerId);
      break;
    }
  }
}


// TODO(benh): Move into stout/hashset.hpp
template <typename Elem>
hashset<Elem> merge(const std::list<hashset<Elem> >& list)
{
  hashset<Elem> result;
  foreach (const hashset<Elem>& set, list) {
    result.insert(set.begin(), set.end());
  }
  return result;
}


Future<hashset<ContainerID> > ComposingContainerizerProcess::containers()
{
  return merge(containers_.values());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
