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

#include <mesos/type_utils.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>

#include "master/maintenance.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace maintenance {

using namespace mesos::maintenance;

using google::protobuf::RepeatedPtrField;

UpdateSchedule::UpdateSchedule(
    const maintenance::Schedule& _schedule)
  : schedule(_schedule) {}


Try<bool> UpdateSchedule::perform(
    Registry* registry,
    hashset<SlaveID>* /*slaveIDs*/)
{
  // Put the machines in the existing schedule into a set.
  hashset<MachineID> existing;
  foreach (const maintenance::Schedule& agenda, registry->schedules()) {
    foreach (const maintenance::Window& window, agenda.windows()) {
      foreach (const MachineID& id, window.machine_ids()) {
        existing.insert(id);
      }
    }
  }

  // Put the machines in the updated schedule into a set.
  // Keep the relevant unavailability to help update existing machines.
  hashmap<MachineID, Unavailability> updated;
  foreach (const maintenance::Window& window, schedule.windows()) {
    foreach (const MachineID& id, window.machine_ids()) {
      updated[id] = window.unavailability();
    }
  }

  // This operation overwrites the existing schedules with a new one.
  // At the same time, every machine in the schedule is saved as a
  // `MachineInfo` in the registry.

  // TODO(josephw): allow more than one schedule.

  // Loop through and modify the existing `MachineInfo` entries.
  for (int i = registry->machines().machines().size() - 1; i >= 0; i--) {
    const MachineID& id = registry->machines().machines(i).info().id();

    // Update the `MachineInfo` entry for all machines in the schedule.
    if (updated.contains(id)) {
      registry->mutable_machines()->mutable_machines(i)->mutable_info()
        ->mutable_unavailability()->CopyFrom(updated[id]);

      continue;
    }

    // Delete the `MachineInfo` entry for each removed machine.
    registry->mutable_machines()->mutable_machines()->DeleteSubrange(i, 1);
  }

  // Create new `MachineInfo` entries for each new machine.
  foreach (const maintenance::Window& window, schedule.windows()) {
    foreach (const MachineID& id, window.machine_ids()) {
      if (existing.contains(id)) {
        continue;
      }

      // Each newly scheduled machine starts in `DRAINING` mode.
      Registry::Machine* machine = registry->mutable_machines()->add_machines();
      MachineInfo* info = machine->mutable_info();
      info->mutable_id()->CopyFrom(id);
      info->set_mode(MachineInfo::DRAINING);
      info->mutable_unavailability()->CopyFrom(window.unavailability());
    }
  }

  // Replace the old schedule with the new schedule.
  registry->clear_schedules();
  registry->add_schedules()->CopyFrom(schedule);

  return true; // Mutation.
}


StartMaintenance::StartMaintenance(
    const RepeatedPtrField<MachineID>& _ids)
{
  foreach (const MachineID& id, _ids) {
    ids.insert(id);
  }
}


Try<bool> StartMaintenance::perform(
    Registry* registry,
    hashset<SlaveID>* /*perform*/)
{
  // Flip the mode of all targeted machines.
  bool changed = false;
  for (int i = 0; i < registry->machines().machines().size(); i++) {
    if (ids.contains(registry->machines().machines(i).info().id())) {
      // Flip the mode.
      registry->mutable_machines()->mutable_machines(i)
        ->mutable_info()->set_mode(MachineInfo::DOWN);

      changed = true; // Mutation.
    }
  }

  return changed;
}


StopMaintenance::StopMaintenance(
    const RepeatedPtrField<MachineID>& _ids)
{
  foreach (const MachineID& id, _ids) {
    ids.insert(id);
  }
}


Try<bool> StopMaintenance::perform(
    Registry* registry,
    hashset<SlaveID>* /*slaveIDs*/)
{
  // Delete the machine info entry of all targeted machines.
  // i.e. Transition them into `UP` mode.
  bool changed = false;
  for (int i = registry->machines().machines().size() - 1; i >= 0; i--) {
    if (ids.contains(registry->machines().machines(i).info().id())) {
      registry->mutable_machines()->mutable_machines()->DeleteSubrange(i, 1);

      changed = true; // Mutation.
    }
  }

  // Delete the machines from the schedule.
  for (int i = registry->schedules().size() - 1; i >= 0; i--) {
    maintenance::Schedule* schedule = registry->mutable_schedules(i);

    for (int j = schedule->windows().size() - 1; j >= 0; j--) {
      maintenance::Window* window = schedule->mutable_windows(j);

      // Delete individual machines.
      for (int k = window->machine_ids().size() - 1; k >= 0; k--) {
        if (ids.contains(window->machine_ids(k))) {
          window->mutable_machine_ids()->DeleteSubrange(k, 1);
        }
      }

      // If the resulting window is empty, delete it.
      if (window->machine_ids().size() == 0) {
        schedule->mutable_windows()->DeleteSubrange(j, 1);
      }
    }

    // If the resulting schedule is empty, delete it.
    if (schedule->windows().size() == 0) {
      registry->mutable_schedules()->DeleteSubrange(i, 1);
    }
  }

  return changed;
}


namespace validation {

Try<Nothing> schedule(
    const maintenance::Schedule& schedule,
    const hashmap<MachineID, Machine>& machines)
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
            "Machine '" + stringify(JSON::protobuf(id)) +
              "' appears more than once in the schedule");
      }

      updated.insert(id);
    }
  }

  // Ensure that no `DOWN` machine is removed from the schedule.
  foreachpair (const MachineID& id, const Machine& machine, machines) {
    if (machine.info.mode() == MachineInfo::DOWN && !updated.contains(id)) {
      return Error(
          "Machine '" + stringify(JSON::protobuf(id)) +
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


Try<Nothing> machines(const RepeatedPtrField<MachineID>& ids)
{
  if (ids.size() <= 0) {
    return Error("List of machines is empty");
  }

  // Verify that the machine has at least one non-empty field value.
  hashset<MachineID> uniques;
  foreach (const MachineID& id, ids) {
    // Validate the single machine.
    Try<Nothing> validId = validation::machine(id);
    if (validId.isError()) {
      return Error(validId.error());
    }

    // Check machine uniqueness.
    if (uniques.contains(id)) {
      return Error(
          "Machine '" + stringify(JSON::protobuf(id)) +
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
