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

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/appc/runtime.hpp"

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

AppcRuntimeIsolatorProcess::AppcRuntimeIsolatorProcess(const Flags& _flags)
  : flags(_flags) {}


AppcRuntimeIsolatorProcess::~AppcRuntimeIsolatorProcess() {}


Try<Isolator*> AppcRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new AppcRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Option<ContainerLaunchInfo>> AppcRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return None();
}


Option<Environment> AppcRuntimeIsolatorProcess::getLaunchEnvironment(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return None();
}


// This method reads the CommandInfo from ExecutorInfo and optional
// TaskInfo, and merge them with Appc image default 'exec'.
Result<CommandInfo> AppcRuntimeIsolatorProcess::getLaunchCommand(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  return None();
}


Option<string> AppcRuntimeIsolatorProcess::getWorkingDirectory(
    const mesos::slave::ContainerConfig& containerConfig)
{
  return None();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
