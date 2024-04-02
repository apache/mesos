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


#include <utility>

#include <stout/error.hpp>

#include "slave/containerizer/mesos/isolators/cgroups2/controller.hpp"

using mesos::slave::ContainerLimitation;

using process::Future;
using process::Owned;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Controller::Controller(Owned<ControllerProcess> _process)
  : process(std::move(_process))
{
  process::spawn(process.get());
}


Controller::~Controller()
{
  process::terminate(process.get());
  process::wait(process.get());
}


string Controller::name() const
{
  return process->name();
}


Future<Nothing> Controller::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::recover,
      containerId,
      cgroup);
}


Future<Nothing> Controller::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::prepare,
      containerId,
      cgroup,
      containerConfig);
}


Future<Nothing> Controller::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::isolate,
      containerId,
      cgroup,
      pid);
}


Future<mesos::slave::ContainerLimitation> Controller::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::watch,
      containerId,
      cgroup);
}


Future<Nothing> Controller::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::update,
      containerId,
      cgroup,
      resourceRequests,
      resourceLimits);
}


Future<ResourceStatistics> Controller::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::usage,
      containerId,
      cgroup);
}


Future<ContainerStatus> Controller::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::status,
      containerId,
      cgroup);
}


Future<Nothing> Controller::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  return process::dispatch(
      process.get(),
      &ControllerProcess::cleanup,
      containerId,
      cgroup);
}


ControllerProcess::ControllerProcess(const Flags& _flags) : flags(_flags) {}


Future<Nothing> ControllerProcess::recover(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}


Future<Nothing> ControllerProcess::prepare(
    const ContainerID& containerId,
    const string& cgroup,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return Nothing();
}


Future<Nothing> ControllerProcess::isolate(
    const ContainerID& containerId,
    const string& cgroup,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> ControllerProcess::watch(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> ControllerProcess::update(
    const ContainerID& containerId,
    const string& cgroup,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return Nothing();
}


Future<ResourceStatistics> ControllerProcess::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ResourceStatistics();
}


Future<ContainerStatus> ControllerProcess::status(
    const ContainerID& containerId,
    const string& cgroup)
{
  return ContainerStatus();
}


Future<Nothing> ControllerProcess::cleanup(
    const ContainerID& containerId,
    const string& cgroup)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
