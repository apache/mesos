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

#include "slave/containerizer/mesos/isolators/cgroups2/controllers/perf_event.hpp"

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/reap.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>

#include "linux/perf.hpp"
#include "slave/containerizer/mesos/isolators/cgroups2/constants.hpp"

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Time;

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<process::Owned<ControllerProcess>> PerfEventControllerProcess::create(
  const Flags& flags)
{
  if (flags.perf_events.isNone()) {
    return Owned<ControllerProcess>(
      new PerfEventControllerProcess(flags, set<string>{}));
  }

  if (!perf::supported()) {
    return Error("Perf is not supported");
  }

  if (flags.perf_duration > flags.perf_interval) {
    return Error(
      "Sampling perf for duration (" + stringify(flags.perf_duration) + ") > "
      "interval (" + stringify(flags.perf_interval) + ") is not supported.");
  }

  set<string> events;
  foreach (const string& event, strings::tokenize(*flags.perf_events, ",")) {
    events.insert(event);
  }

  if (!perf::valid(events)) {
    return Error("Invalid perf events: " + stringify(events));
  }

  LOG(INFO) << "perf_event controller will profile for "
            << "'" << flags.perf_duration << "' "
            << "every '" << flags.perf_interval << "' "
            << "for events: " << stringify(events);

  return Owned<ControllerProcess>(
    new PerfEventControllerProcess(flags, events));
}


PerfEventControllerProcess::PerfEventControllerProcess(
  const Flags& _flags, const std::set<std::string>& _events)
  : ProcessBase(process::ID::generate("cgroups-v2-perf-event-controller")),
    ControllerProcess(_flags),
    events(_events) {}


string PerfEventControllerProcess::name() const
{
  return CGROUPS2_CONTROLLER_PERF_EVENT_NAME;
}

void PerfEventControllerProcess::initialize()
{
  // Start sampling.
  if (!events.empty()) {
    sample();
  }
}


Future<Nothing> PerfEventControllerProcess::recover(
  const ContainerID& containerId, const string& cgroup)
{
  if (infos.contains(containerId)) {
    return Failure("The controller '" + name() + "' has already been recovered");
  }

  infos.put(containerId, Owned<Info>(new Info(cgroup)));

  return Nothing();
}


Future<Nothing> PerfEventControllerProcess::prepare(
  const ContainerID& containerId,
  const string& cgroup,
  const mesos::slave::ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("The controller '" + name() + "' has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info(cgroup)));

  return Nothing();
}


Future<ResourceStatistics> PerfEventControllerProcess::usage(
  const ContainerID& containerId, const string& cgroup)
{
  if (!infos.contains(containerId)) {
    return Failure(
      "Failed to get the usage of controller '" + name() +
      "'"
      ": Unknown container");
  }

  ResourceStatistics statistics;
  statistics.mutable_perf()->CopyFrom(infos[containerId]->statistics);

  return statistics;
}


Future<Nothing> PerfEventControllerProcess::cleanup(
  const ContainerID& containerId, const string& cgroup)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup controller '" << name() << "' "
            << "request for unknown container " << containerId;

    return Nothing();
  }

  infos.erase(containerId);

  return Nothing();
}


void PerfEventControllerProcess::sample()
{
  // Collect a perf sample for all cgroups that are not being
  // destroyed. Since destroyal is asynchronous, 'perf stat' may
  // fail if the cgroup is destroyed before running perf.
  set<string> cgroups;

  foreachvalue (const Owned<Info>& info, infos) {
    cgroups.insert(info->cgroup);
  }

  // The discard timeout includes an allowance of twice the
  // reaper interval to ensure we see the perf process exit.
  Duration timeout = flags.perf_duration + process::MAX_REAP_INTERVAL() * 2;
  Duration duration = flags.perf_duration;

  perf::sample(events, cgroups, flags.perf_duration)
    .after(timeout, [=](Future<hashmap<string, PerfStatistics>> future) {
      LOG(ERROR) << "Perf sample of " << stringify(duration)
                 << " failed to complete within " << stringify(timeout)
                 << "; sampling will be halted";

      future.discard();

      return future;
    })
    .onAny(defer(PID<PerfEventControllerProcess>(this),
                 &PerfEventControllerProcess::_sample,
                 Clock::now() + flags.perf_interval,
                 lambda::_1));
}


void PerfEventControllerProcess::_sample(
  const Time& next, const Future<hashmap<string, PerfStatistics>>& statistics)
{
  if (!statistics.isReady()) {
    // In case the failure is transient or this is due to a timeout,
    // we continue sampling. Note that since sampling is done on an
    // interval, it should be ok if this is a non-transient failure.
    LOG(ERROR) << "Failed to get the perf sample: "
               << (statistics.isFailed() ? statistics.failure() : "timeout");
  } else {
    // Store the latest statistics, note that cgroups added in the
    // interim will be picked up by the next sample.
    foreachvalue (const Owned<Info>& info, infos) {
      if (statistics->contains(info->cgroup)) {
        info->statistics = statistics->get(info->cgroup).get();
      }
    }
  }

  // Schedule sample for the next time.
  delay(next - Clock::now(),
        PID<PerfEventControllerProcess>(this),
        &PerfEventControllerProcess::sample);
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
