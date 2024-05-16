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

#include <string>

#include "linux/cgroups2.hpp"

#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/controllers/cpu.hpp"

#include <process/id.hpp>
#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "logging/logging.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Owned;

using cgroups2::cpu::BandwidthLimit;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<ControllerProcess>> CpuControllerProcess::create(const Flags& flags)
{
  return Owned<ControllerProcess>(new CpuControllerProcess(flags));
}


CpuControllerProcess::CpuControllerProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("cgroups-v2-cpu-controller")),
    ControllerProcess(_flags) {}


string CpuControllerProcess::name() const
{
  return CGROUPS2_CONTROLLER_CPU_NAME;
}


Future<Nothing> CpuControllerProcess::update(
  const ContainerID& containerId,
  const string& cgroup,
  const Resources& resourceRequests,
  const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  if (resourceRequests.cpus().isNone()) {
    return Failure(
        "Failed to update the 'cpu' controller: No cpu resources requested");
  }

  // Handle CPU request:
  // Compute and update the CPU weight for this cgroup. Weight is the product of
  // the requested number of CPUs and the pre-set weight per CPU. If the
  // `revocable_cpu_low_priority` flag is set, less weight is given per cpu.
  double cpuRequest = *resourceRequests.cpus();
  bool revocable = resourceRequests.revocable().cpus().isSome();
  uint64_t weightPerCpu = (revocable && flags.revocable_cpu_low_priority) ?
      CGROUPS2_CPU_WEIGHT_PER_CPU_REVOCABLE : CGROUPS2_CPU_WEIGHT_PER_CPU;
  uint64_t weight = std::max(
      static_cast<uint64_t>(weightPerCpu * cpuRequest), CGROUPS2_MIN_CPU_WEIGHT);

  Try<Nothing> update = cgroups2::cpu::weight(cgroup, weight);
  if (update.isError()) {
    return Failure("Failed to update the weight: " + update.error());
  }

  // Handle CPU limit (if any):
  //
  // (1) If a cpu limit is provided, it's used to set the limit. Otherwise,
  // (2) limit = request (--cgroups_enable_cfs=true), or
  // (3) no limit (--cgroups_enable_cfs=false).
  Option<double> cpuLimit;
  if (resourceLimits.count("cpus") > 0) {
    cpuLimit = resourceLimits.at("cpus").value();
  }
  BandwidthLimit limit = [&] () {
    uint64_t min_quota = static_cast<uint64_t>(CGROUPS2_MIN_CPU_CFS_QUOTA.us());
    if (cpuLimit.isSome()) {
      if (std::isinf(*cpuLimit)) {
        return BandwidthLimit(); // (1)
      }

      uint64_t quota = static_cast<uint64_t>(*cpuLimit * CGROUPS2_CPU_CFS_PERIOD.us());
      return BandwidthLimit(
          Microseconds(std::max(quota, min_quota)),
          CGROUPS2_CPU_CFS_PERIOD); // (1)
    }

    if (flags.cgroups_enable_cfs) {
      uint64_t quota = static_cast<uint64_t>(cpuRequest * CGROUPS2_CPU_CFS_PERIOD.us());
      return BandwidthLimit(
          Microseconds(std::max(quota, min_quota)),
          CGROUPS2_CPU_CFS_PERIOD); // (2)
    }

    return BandwidthLimit(); // (3)
  }();

  update = cgroups2::cpu::set_max(cgroup, limit);
  if (update.isError()) {
    return Failure("Failed to set bandwidth limit for cgroup '" + cgroup + "': "
                   + update.error());
  }

  return Nothing();
}


Future<ResourceStatistics> CpuControllerProcess::usage(
    const ContainerID& _containerId,
    const string& cgroup)
{
  Try<cgroups2::cpu::Stats> stats = cgroups2::cpu::stats(cgroup);
  if (stats.isError()) {
    return Failure("Failed to get CPU stats for '" + cgroup + "': "
                   + stats.error());
  }

  ResourceStatistics usage;
  usage.set_cpus_user_time_secs(stats->user_time.secs());
  usage.set_cpus_system_time_secs(stats->system_time.secs());

  if (stats->periods.isSome()) {
    usage.set_cpus_nr_periods(*stats->periods);
  }
  if (stats->throttled.isSome()) {
    usage.set_cpus_nr_throttled(*stats->throttled);
  }
  if (stats->throttle_time.isSome()) {
    usage.set_cpus_throttled_time_secs(stats->throttle_time->secs());
  }

  if (stats->periods.isNone()
      || stats->throttled.isNone()
      || stats->throttle_time.isNone()) {
    LOG(ERROR) << "cpu throttling stats missing for cgroup '" << cgroup << "'"
                  " despite the 'cpu' controller being enabled";
  }

  return usage;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
