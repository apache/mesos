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

#ifndef __MESOS_MASTER_MAINTENANCE_HPP__
#define __MESOS_MASTER_MAINTENANCE_HPP__

#include <mesos/mesos.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "master/registrar.hpp"
#include "master/registry.hpp"

namespace mesos {
namespace internal {
namespace master {
namespace maintenance {
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
    const hashmap<MachineID, MachineInfo>& infos);


// Checks that the `duration` of the unavailability is non-negative.
Try<Nothing> unavailability(
    const Unavailability& unavailability);


/**
 * Performs the following checks on a list of machines:
 *   - Each machine appears in the list once and only once.
 *   - The list is non-empty.
 *   - All checks in the `machine` method below.
 */
Try<Nothing> machines(const MachineIDs& ids);


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
