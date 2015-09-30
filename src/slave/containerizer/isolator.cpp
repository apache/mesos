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

#include <process/dispatch.hpp>

#include "slave/containerizer/isolator.hpp"

using namespace process;

using std::string;
using std::list;

using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerPrepareInfo;
using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

MesosIsolator::MesosIsolator(Owned<MesosIsolatorProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


MesosIsolator::~MesosIsolator()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> MesosIsolator::recover(
    const list<ContainerState>& state,
    const hashset<ContainerID>& orphans)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::recover,
                  state,
                  orphans);
}


Future<Option<ContainerPrepareInfo>> MesosIsolator::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::prepare,
                  containerId,
                  executorInfo,
                  directory,
                  user);
}


Future<Nothing> MesosIsolator::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::isolate,
                  containerId,
                  pid);
}


Future<ContainerLimitation> MesosIsolator::watch(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::watch,
                  containerId);
}


Future<Nothing> MesosIsolator::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> MesosIsolator::usage(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::usage,
                  containerId);
}


Future<Nothing> MesosIsolator::cleanup(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::cleanup,
                  containerId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
