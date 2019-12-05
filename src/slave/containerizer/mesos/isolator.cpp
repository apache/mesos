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

#include <process/dispatch.hpp>

#include "slave/containerizer/mesos/isolator.hpp"

using namespace process;

using std::string;
using std::vector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
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


bool MesosIsolator::supportsNesting()
{
  return process->supportsNesting();
}


bool MesosIsolator::supportsStandalone()
{
  return process->supportsStandalone();
}


Future<Nothing> MesosIsolator::recover(
    const vector<ContainerState>& state,
    const hashset<ContainerID>& orphans)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::recover,
                  state,
                  orphans);
}


Future<Option<ContainerLaunchInfo>> MesosIsolator::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::prepare,
                  containerId,
                  containerConfig);
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
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::update,
                  containerId,
                  resourceRequests,
                  resourceLimits);
}


Future<ResourceStatistics> MesosIsolator::usage(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::usage,
                  containerId);
}


Future<ContainerStatus> MesosIsolator::status(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosIsolatorProcess::status,
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
