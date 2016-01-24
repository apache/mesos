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

#include <stdint.h>

#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/pid.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"
#include "linux/perf.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/perf_event.hpp"

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using std::list;
using std::set;
using std::string;
using std::vector;

using process::Clock;
using process::Failure;
using process::Future;
using process::PID;
using process::Time;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> CgroupsPerfEventIsolatorProcess::create(const Flags& flags)
{
  LOG(INFO) << "Creating PerfEvent isolator";

  if (!perf::supported()) {
    return Error("Perf is not supported");
  }

  if (flags.perf_duration > flags.perf_interval) {
    return Error("Sampling perf for duration (" +
                 stringify(flags.perf_duration) +
                 ") > interval (" +
                 stringify(flags.perf_interval) +
                 ") is not supported.");
  }

  if (!flags.perf_events.isSome()) {
    return Error("No perf events specified");
  }

  set<string> events;
  foreach (const string& event,
           strings::tokenize(flags.perf_events.get(), ",")) {
    events.insert(event);
  }

  if (!perf::valid(events)) {
    return Error("Failed to create PerfEvent isolator, invalid events: " +
                 stringify(events));
  }

  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "perf_event",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create perf_event cgroup: " + hierarchy.error());
  }

  LOG(INFO) << "PerfEvent isolator will profile for " << flags.perf_duration
            << " every " << flags.perf_interval
            << " for events: " << stringify(events);

  process::Owned<MesosIsolatorProcess> process(
      new CgroupsPerfEventIsolatorProcess(flags, hierarchy.get(), events));

  return new MesosIsolator(process);
}


CgroupsPerfEventIsolatorProcess::~CgroupsPerfEventIsolatorProcess() {}


void CgroupsPerfEventIsolatorProcess::initialize()
{
  // Start sampling.
  sample();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }

      infos.clear();
      return Failure("Failed to check cgroup " + cgroup +
                     " for container '" + stringify(containerId) + "'");
    }

    if (!exists.get()) {
      // This may occur if the executor is exiting and the isolator has
      // destroyed the cgroup but the slave dies before noticing this. This
      // will be detected when the containerizer tries to monitor the
      // executor's pid.
      // NOTE: This could also occur if this isolator is now enabled for a
      // container that was started without this isolator. For this
      // particular isolator it is acceptable to continue running this
      // container without a perf_event cgroup because we don't ever
      // query it and the destroy will succeed immediately.
      VLOG(1) << "Couldn't find perf event cgroup for container " << containerId
              << ", perf statistics will not be available";
      continue;
    }

    infos[containerId] = new Info(containerId, cgroup);
  }

  // Remove orphan cgroups.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
    foreachvalue (Info* info, infos) {
      delete info;
    }
    infos.clear();
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Ignore the slave cgroup (see the --slave_subsystems flag).
    // TODO(idownes): Remove this when the cgroups layout is updated,
    // see MESOS-1185.
    if (cgroup == path::join(flags.cgroups_root, "slave")) {
      continue;
    }

    ContainerID containerId;
    containerId.set_value(Path(cgroup).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    // Known orphan cgroups will be destroyed by the containerizer
    // using the normal cleanup path. See details in MESOS-2367.
    if (orphans.contains(containerId)) {
      infos[containerId] = new Info(containerId, cgroup);
      continue;
    }

    LOG(INFO) << "Removing unknown orphaned cgroup '" << cgroup << "'";

    // We don't wait on the destroy as we don't want to block recovery.
    cgroups::destroy(hierarchy, cgroup, cgroups::DESTROY_TIMEOUT);
  }

  return Nothing();
}


Future<Option<ContainerLaunchInfo>> CgroupsPerfEventIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  LOG(INFO) << "Preparing perf event cgroup for " << containerId;

  Info* info = new Info(
      containerId,
      path::join(flags.cgroups_root, containerId.value()));

  infos[containerId] = CHECK_NOTNULL(info);

  // Create a cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchy, info->cgroup);

  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  }

  if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  if (!exists.get()) {
    Try<Nothing> create = cgroups::create(hierarchy, info->cgroup);
    if (create.isError()) {
      return Failure("Failed to prepare isolator: " + create.error());
    }
  }

  // Chown the cgroup so the executor can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  if (containerConfig.has_user()) {
    Try<Nothing> chown = os::chown(
        containerConfig.user(),
        path::join(hierarchy, info->cgroup),
        false);
    if (chown.isError()) {
      return Failure("Failed to prepare isolator: " + chown.error());
    }
  }

  return None();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  Try<Nothing> assign = cgroups::assign(hierarchy, info->cgroup, pid);
  if (assign.isError()) {
    return Failure("Failed to assign container '" +
                   stringify(info->containerId) + "' to its own cgroup '" +
                   path::join(hierarchy, info->cgroup) +
                   "' : " + assign.error());
  }

  return Nothing();
}


Future<ContainerLimitation> CgroupsPerfEventIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // No resources are limited.
  return Future<ContainerLimitation>();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  // Nothing to update.
  return Nothing();
}


Future<ResourceStatistics> CgroupsPerfEventIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    // Return an empty ResourceStatistics, i.e., without
    // PerfStatistics, if we don't know about this container.
    return ResourceStatistics();
  }

  CHECK_NOTNULL(infos[containerId]);

  ResourceStatistics statistics;
  statistics.mutable_perf()->CopyFrom(infos[containerId]->statistics);

  return statistics;
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Tolerate clean up attempts for unknown containers which may arise from
  // repeated clean up attempts (during test cleanup).
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;
    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  info->destroying = true;

  return cgroups::destroy(hierarchy, info->cgroup)
    .then(defer(PID<CgroupsPerfEventIsolatorProcess>(this),
                &CgroupsPerfEventIsolatorProcess::_cleanup,
                containerId));
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::_cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Nothing();
  }

  delete infos[containerId];
  infos.erase(containerId);

  return Nothing();
}


Future<hashmap<string, PerfStatistics>> discardSample(
    Future<hashmap<string, PerfStatistics>> future,
    const Duration& duration,
    const Duration& timeout)
{
  LOG(ERROR) << "Perf sample of " << stringify(duration)
             << " failed to complete within " << stringify(timeout)
             << "; sampling will be halted";

  future.discard();

  return future;
}


void CgroupsPerfEventIsolatorProcess::sample()
{
  // Collect a perf sample for all cgroups that are not being
  // destroyed. Since destroyal is asynchronous, 'perf stat' may
  // fail if the cgroup is destroyed before running perf.
  set<string> cgroups;

  foreachvalue (Info* info, infos) {
    CHECK_NOTNULL(info);

    if (!info->destroying) {
      cgroups.insert(info->cgroup);
    }
  }

  // The discard timeout includes an allowance of twice the
  // reaper interval to ensure we see the perf process exit.
  Duration timeout = flags.perf_duration + process::MAX_REAP_INTERVAL() * 2;

  perf::sample(events, cgroups, flags.perf_duration)
    .after(timeout,
           lambda::bind(&discardSample,
                        lambda::_1,
                        flags.perf_duration,
                        timeout))
    .onAny(defer(PID<CgroupsPerfEventIsolatorProcess>(this),
                 &CgroupsPerfEventIsolatorProcess::_sample,
                 Clock::now() + flags.perf_interval,
                 lambda::_1));
}


void CgroupsPerfEventIsolatorProcess::_sample(
    const Time& next,
    const Future<hashmap<string, PerfStatistics>>& statistics)
{
  if (!statistics.isReady()) {
    // In case the failure is transient or this is due to a timeout,
    // we continue sampling. Note that since sampling is done on an
    // interval, it should be ok if this is a non-transient failure.
    LOG(ERROR) << "Failed to get perf sample: "
               << (statistics.isFailed()
                   ? statistics.failure()
                   : "discarded due to timeout");
  } else {
    // Store the latest statistics, note that cgroups added in the
    // interim will be picked up by the next sample.
    foreachvalue (Info* info, infos) {
      CHECK_NOTNULL(info);

      if (statistics->contains(info->cgroup)) {
        info->statistics = statistics->get(info->cgroup).get();
      }
    }
  }

  // Schedule sample for the next time.
  delay(next - Clock::now(),
        PID<CgroupsPerfEventIsolatorProcess>(this),
        &CgroupsPerfEventIsolatorProcess::sample);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
