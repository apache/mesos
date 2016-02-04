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
#include <string>

#include <glog/logging.h>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerRuntimeIsolatorProcess::DockerRuntimeIsolatorProcess(
    const Flags& _flags)
  : flags(_flags) {}


DockerRuntimeIsolatorProcess::~DockerRuntimeIsolatorProcess() {}


Try<Isolator*> DockerRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new DockerRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Nothing> DockerRuntimeIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<Option<ContainerLaunchInfo>> DockerRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  const ExecutorInfo& executorInfo = containerConfig.executor_info();

  if (!executorInfo.has_container()) {
    return None();
  }

  if (executorInfo.container().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare docker runtime for a MESOS contaienr");
  }

  if (!containerConfig.has_docker()) {
    // No docker image default config available.
    return None();
  }

  // Contains docker image default environment variables, merged
  // command, and working directory.
  ContainerLaunchInfo launchInfo;

  Option<Environment> environment = getLaunchEnvironment(
      containerId,
      containerConfig);

  if (environment.isSome()) {
    launchInfo.mutable_environment()->CopyFrom(environment.get());
  }

  return launchInfo;
}


Option<Environment> DockerRuntimeIsolatorProcess::getLaunchEnvironment(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  if (containerConfig.docker().manifest().config().env_size() == 0) {
    return None();
  }

  Environment environment;

  foreach(const string& env,
          containerConfig.docker().manifest().config().env()) {
    // Use `find_first_of` to prevent to case that there are
    // multiple '=' existing.
    size_t position = env.find_first_of('=');
    if (position == string::npos) {
      VLOG(1) << "Skipping invalid environment variable: '"
              << env << "' in docker manifest for container "
              << containerId;

      continue;
    }

    const string name = env.substr(0, position);
    const string value = env.substr(position + 1);

    // Keep all environment from runtime isolator. If there exists
    // environment variable duplicate cases, it will be overwritten
    // in mesos containerizer.
    Environment::Variable* variable = environment.add_variables();
    variable->set_name(name);
    variable->set_value(value);
  }

  return environment;
}


Future<Nothing> DockerRuntimeIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> DockerRuntimeIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> DockerRuntimeIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> DockerRuntimeIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return ResourceStatistics();
}


Future<Nothing> DockerRuntimeIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
