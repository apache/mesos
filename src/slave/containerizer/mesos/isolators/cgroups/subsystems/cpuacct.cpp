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

#include <process/id.hpp>

#include "linux/cgroups.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/subsystems/cpuacct.hpp"

using process::Failure;
using process::Future;
using process::Owned;

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<SubsystemProcess>> CpuacctSubsystemProcess::create(
    const Flags& flags,
    const string& hierarchy)
{
  return Owned<SubsystemProcess>(new CpuacctSubsystemProcess(flags, hierarchy));
}


CpuacctSubsystemProcess::CpuacctSubsystemProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : ProcessBase(process::ID::generate("cgroups-cpuacct-subsystem")),
    SubsystemProcess(_flags, _hierarchy) {}


Future<ResourceStatistics> CpuacctSubsystemProcess::usage(
    const ContainerID& containerId,
    const string& cgroup)
{
  ResourceStatistics result;

  // TODO(chzhcn): Getting the number of processes and threads is
  // available as long as any cgroup subsystem is used so this best
  // not be tied to a specific cgroup subsystem. A better place is
  // probably Linux Launcher, which uses the cgroup freezer subsystem.
  // That requires some change for it to adopt the new semantics of
  // reporting subsystem-independent cgroup usage.
  // Note: The complexity of this operation is linear to the number of
  // processes and threads in a container: the kernel has to allocate
  // memory to contain the list of pids or tids; the userspace has to
  // parse the cgroup files to get the size. If this proves to be a
  // performance bottleneck, some kind of rate limiting mechanism
  // needs to be employed.
  if (flags.cgroups_cpu_enable_pids_and_tids_count) {
    Try<set<pid_t>> pids = cgroups::processes(hierarchy, cgroup);

    if (pids.isError()) {
      return Failure("Failed to get number of processes: " + pids.error());
    }

    result.set_processes(pids->size());

    Try<set<pid_t>> tids = cgroups::threads(hierarchy, cgroup);

    if (tids.isError()) {
      return Failure("Failed to get number of threads: " + tids.error());
    }

    result.set_threads(tids->size());
  }

  // Get the number of clock ticks, used for cpu accounting.
  static long ticks = sysconf(_SC_CLK_TCK);

  PCHECK(ticks > 0) << "Failed to get sysconf(_SC_CLK_TCK)";

  // Add the cpuacct.stat information.
  Try<hashmap<string, uint64_t>> stat = cgroups::stat(
      hierarchy,
      cgroup,
      "cpuacct.stat");

  if (stat.isError()) {
    return Failure("Failed to read 'cpuacct.stat': " + stat.error());
  }

  // TODO(bmahler): Add namespacing to cgroups to enforce the expected
  // structure, e.g., cgroups::cpuacct::stat.
  Option<uint64_t> user = stat->get("user");
  Option<uint64_t> system = stat->get("system");

  if (user.isSome() && system.isSome()) {
    result.set_cpus_user_time_secs((double) user.get() / (double) ticks);
    result.set_cpus_system_time_secs((double) system.get() / (double) ticks);
  }

  return result;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
