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

#include <mesos/slave/isolator.hpp>

#include <process/dispatch.hpp>

using namespace process;

using std::string;
using std::list;

namespace mesos {
namespace slave {

Isolator::Isolator(Owned<IsolatorProcess> _process)
  : process(_process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


Isolator::~Isolator()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Option<int>> Isolator::namespaces()
{
  return dispatch(process.get(), &IsolatorProcess::namespaces);
}


Future<Nothing> Isolator::recover(
    const list<ExecutorRunState>& state,
    const hashset<ContainerID>& orphans)
{
  return dispatch(process.get(),
                  &IsolatorProcess::recover,
                  state,
                  orphans);
}


Future<Option<CommandInfo>> Isolator::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& rootfs,
    const Option<string>& user)
{
  return dispatch(process.get(),
                  &IsolatorProcess::prepare,
                  containerId,
                  executorInfo,
                  directory,
                  rootfs,
                  user);
}


Future<Nothing> Isolator::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return dispatch(process.get(), &IsolatorProcess::isolate, containerId, pid);
}


Future<ExecutorLimitation> Isolator::watch(const ContainerID& containerId)
{
  return dispatch(process.get(), &IsolatorProcess::watch, containerId);
}


Future<Nothing> Isolator::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(
      process.get(),
      &IsolatorProcess::update,
      containerId,
      resources);
}


Future<ResourceStatistics> Isolator::usage(
    const ContainerID& containerId) const
{
  return dispatch(process.get(), &IsolatorProcess::usage, containerId);
}


Future<Nothing> Isolator::cleanup(const ContainerID& containerId)
{
  return dispatch(process.get(), &IsolatorProcess::cleanup, containerId);
}

} // namespace slave {
} // namespace mesos {
