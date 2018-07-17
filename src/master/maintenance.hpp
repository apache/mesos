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

#ifndef __MESOS_MASTER_MAINTENANCE_HPP__
#define __MESOS_MASTER_MAINTENANCE_HPP__

#include <mesos/mesos.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "master/machine.hpp"
#include "master/registrar.hpp"
#include "master/registry.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace maintenance {

// Maintenance registry operations will never report a failure while
// performing the operation (i.e. `perform` never returns an `Error`).
// This means the underlying `Future` for the operation will always be
// set to true.  If there is a separate failure, such as a network
// partition, while performing the operation, this future will not be set.

/**
 * Updates the maintanence schedule of the cluster.  This transitions machines
 * between `UP` and `DRAINING` modes only.  The given schedule must only
 * add valid machines and remove machines that are not `DOWN`.
 *
 * TODO(josephw): allow more than one schedule.
 */
class UpdateSchedule : public RegistryOperation
{
public:
  explicit UpdateSchedule(
      const mesos::maintenance::Schedule& _schedule);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const mesos::maintenance::Schedule schedule;
};


/**
 * Transitions a group of machines from `DRAINING` mode into
 * `DOWN` mode.  All machines must be part of a maintenance
 * schedule prior to executing this operation.
 *
 * TODO(josephw): Allow a transition from `UP` to `DOWN`.
 */
class StartMaintenance : public RegistryOperation
{
public:
  explicit StartMaintenance(
      const google::protobuf::RepeatedPtrField<MachineID>& _ids);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  hashset<MachineID> ids;
};


/**
 * Transitions a group of machines from `DOWN` mode into `UP` mode.
 * All machines must be in `DOWN` mode and must be part of a maintenance
 * schedule prior to executing this operation. The machines will be
 * removed from the maintenance schedule.
 */
class StopMaintenance : public RegistryOperation
{
public:
  explicit StopMaintenance(
      const google::protobuf::RepeatedPtrField<MachineID>& _ids);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  hashset<MachineID> ids;
};


namespace validation {

/**
 * Performs the following checks on the new maintenance schedule:
 *   - Each window in the new schedule has at least one machine.
 *   - All unavailabilities adhere to the `unavailability` method below.
 *   - Each machine appears in the schedule once and only once.
 *   - All currently `DOWN` machines are present in the schedule.
 *   - All checks in the `machine` method below.
 */
Try<Nothing> schedule(
    const mesos::maintenance::Schedule& schedule,
    const hashmap<MachineID, Machine>& machines);


// Checks that the `duration` of the unavailability is non-negative.
Try<Nothing> unavailability(
    const Unavailability& unavailability);


/**
 * Performs the following checks on a list of machines:
 *   - Each machine appears in the list once and only once.
 *   - The list is non-empty.
 *   - All checks in the `machine` method below.
 */
Try<Nothing> machines(const google::protobuf::RepeatedPtrField<MachineID>& ids);


/**
 * Performs the following checks on a single machine:
 *   - The machine has at least a hostname or IP.
 *   - IP is correctly formed.
 */
Try<Nothing> machine(const MachineID& id);

} // namespace validation {
} // namespace maintenance {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_MASTER_MAINTENANCE_HPP__
