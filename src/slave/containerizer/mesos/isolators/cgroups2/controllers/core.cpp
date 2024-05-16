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

#include <set>
#include <string>

#include "linux/cgroups2.hpp"

#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/core.hpp"

#include <process/id.hpp>
#include <process/owned.hpp>

using process::Failure;
using process::Future;
using process::Owned;

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<ControllerProcess>> CoreControllerProcess::create(const Flags& flags)
{
  return Owned<ControllerProcess>(new CoreControllerProcess(flags));
}


CoreControllerProcess::CoreControllerProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("cgroups-v2-core-controller")),
    ControllerProcess(_flags) {}


string CoreControllerProcess::name() const
{
  return CGROUPS2_CONTROLLER_CORE_NAME;
}


process::Future<ResourceStatistics> CoreControllerProcess::usage(
    const ContainerID& containerId,
    const std::string& cgroup)
{
  ResourceStatistics stats;

  if (flags.cgroups_cpu_enable_pids_and_tids_count) {
    Try<set<pid_t>> pids = cgroups2::processes(cgroup);
    if (pids.isError()) {
      return Failure("Failed to get processes in cgroup: " + pids.error());
    }

    stats.set_processes(pids->size());

    Try<set<pid_t>> tids = cgroups2::threads(cgroup);
    if (tids.isError()) {
      return Failure("Failed to get threads in cgroup: " + tids.error());
    }

    stats.set_threads(tids->size());
  }

  return stats;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
