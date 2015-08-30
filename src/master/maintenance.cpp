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

#include <mesos/type_utils.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>

#include "master/maintenance.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace maintenance {

using namespace mesos::maintenance;

namespace validation {

Try<Nothing> schedule(
    const maintenance::Schedule& schedule,
    const hashmap<MachineID, MachineInfo>& infos)
{
  hashset<MachineID> updated;
  foreach (const maintenance::Window& window, schedule.windows()) {
    // Check that each window has at least one machine.
    if (window.machine_ids().size() == 0) {
      return Error("List of machines in the maintenance window is empty");
    }

    // Check the time specified by the unavailability.
    Try<Nothing> unavailability =
      maintenance::validation::unavailability(window.unavailability());

    if (unavailability.isError()) {
      return Error(unavailability.error());
    }

    // Collect machines from the updated schedule into a set.
    foreach (const MachineID& id, window.machine_ids()) {
      // Validate the single machine.
      Try<Nothing> validId = validation::machine(id);
      if (validId.isError()) {
        return Error(validId.error());
      }

      // Check that the machine is unique.
      if (updated.contains(id)) {
        return Error(
            "Machine '" + id.DebugString() +
              "' appears more than once in the schedule");
      }

      updated.insert(id);
    }
  }

  // Ensure that no `DOWN` machine is removed from the schedule.
  foreachpair (const MachineID& id, const MachineInfo& info, infos) {
    if (info.mode() == MachineInfo::DOWN && !updated.contains(id)) {
      return Error(
          "Machine '" + id.DebugString() +
            "' is deactivated and cannot be removed from the schedule");
    }
  }

  return Nothing();
}


Try<Nothing> unavailability(const Unavailability& unavailability)
{
  const Duration duration =
    Nanoseconds(unavailability.duration().nanoseconds());

  // Check the bounds of the unavailability.
  if (duration < Duration::zero()) {
    return Error("Unavailability 'duration' is negative");
  }

  return Nothing();
}


Try<Nothing> machines(const MachineIDs& ids)
{
  if (ids.values().size() <= 0) {
    return Error("List of machines is empty");
  }

  // Verify that the machine has at least one non-empty field value.
  hashset<MachineID> uniques;
  foreach (const MachineID& id, ids.values()) {
    // Validate the single machine.
    Try<Nothing> validId = validation::machine(id);
    if (validId.isError()) {
      return Error(validId.error());
    }

    // Check machine uniqueness.
    if (uniques.contains(id)) {
      return Error(
          "Machine '" + id.DebugString() +
            "' appears more than once in the schedule");
    }

    uniques.insert(id);
  }

  return Nothing();
}


Try<Nothing> machine(const MachineID& id)
{
  // Verify that the machine has at least one non-empty field value.
  if (id.hostname().empty() && id.ip().empty()) {
    return Error("Both 'hostname' and 'ip' for a machine are empty");
  }

  // Validate the IP field.
  if (!id.ip().empty()) {
    Try<net::IP> ip = net::IP::parse(id.ip(), AF_INET);
    if (ip.isError()) {
      return Error(ip.error());
    }
  }

  return Nothing();
}

} // namespace validation {
} // namespace maintenance {
} // namespace master {
} // namespace internal {
} // namespace mesos {
