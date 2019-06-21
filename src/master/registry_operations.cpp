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

#include <stout/check.hpp>

#include "master/registry_operations.hpp"

#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

namespace mesos {
namespace internal {
namespace master {

AdmitSlave::AdmitSlave(const SlaveInfo& _info) : info(_info)
{
  CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
}


Try<bool> AdmitSlave::perform(Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check if this slave is currently admitted. This should only
  // happen if there is a slaveID collision, but that is extremely
  // unlikely in practice: slaveIDs are prefixed with the master ID,
  // which is a randomly generated UUID.
  if (slaveIDs->contains(info.id())) {
    return Error("Agent already admitted");
  }

  // Downgrade the resources such that an older master can recover from
  // the checkpointed registry state.
  CHECK_SOME(downgradeResources(&info));

  Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
  slave->mutable_info()->CopyFrom(info);
  slaveIDs->insert(info.id());
  return true; // Mutation.
}


UpdateSlave::UpdateSlave(const SlaveInfo& _info) : info(_info)
{
  CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
}


Try<bool> UpdateSlave::perform(Registry* registry, hashset<SlaveID>* slaveIDs)
{
  if (!slaveIDs->contains(info.id())) {
    return Error("Agent not yet admitted.");
  }

  for (int i = 0; i < registry->slaves().slaves().size(); i++) {
    Registry::Slave* slave = registry->mutable_slaves()->mutable_slaves(i);

    if (slave->info().id() == info.id()) {
      // The SlaveInfo in the registry is stored in the
      // `PRE_RESERVATION_REFINEMENT` format, but the equality operator
      // asserts that resources are in `POST_RESERVATION_REFINEMENT` format,
      // so we have to upgrade before we can do the comparison.
      SlaveInfo previousInfo(slave->info());
      upgradeResources(&previousInfo);

      if (info == previousInfo) {
        return false; // No mutation.
      }

      // Downgrade the resources such that an older master can recover from
      // the checkpointed registry state.
      CHECK_SOME(downgradeResources(&info));

      slave->mutable_info()->CopyFrom(info);
      return true; // Mutation.
    }
  }

  // Shouldn't happen
  return Error("Failed to find agent " + stringify(info.id()));
}


// Move a slave from the list of admitted slaves to the list of
// unreachable slaves.
MarkSlaveUnreachable::MarkSlaveUnreachable(
    const SlaveInfo& _info,
    const TimeInfo& _unreachableTime)
  : info(_info)
  , unreachableTime(_unreachableTime)
{
  CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
}


Try<bool> MarkSlaveUnreachable::perform(
    Registry* registry,
    hashset<SlaveID>* slaveIDs)
{
  // As currently implemented, this should not be possible: the
  // master will only mark slaves unreachable that are currently
  // admitted.
  if (!slaveIDs->contains(info.id())) {
    return Error("Agent not yet admitted");
  }

  for (int i = 0; i < registry->slaves().slaves().size(); i++) {
    const Registry::Slave& slave = registry->slaves().slaves(i);

    if (slave.info().id() == info.id()) {
      Registry::UnreachableSlave* unreachable =
        registry->mutable_unreachable()->add_slaves();

      unreachable->mutable_id()->CopyFrom(info.id());
      unreachable->mutable_timestamp()->CopyFrom(unreachableTime);

      // Copy the draining and deactivation states.
      if (slave.has_drain_info()) {
        unreachable->mutable_drain_info()->CopyFrom(slave.drain_info());
      }

      unreachable->set_deactivated(slave.deactivated());

      registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
      slaveIDs->erase(info.id());

      return true; // Mutation.
    }
  }

  // Should not happen.
  return Error("Failed to find agent " + stringify(info.id()));
}


// Add a slave back to the list of admitted slaves. The slave will
// typically be in the "unreachable" list; if so, it is removed from
// that list. The slave might also be in the "admitted" list already.
// Finally, the slave might be in neither the "unreachable" or
// "admitted" lists, if its metadata has been garbage collected from
// the registry.
MarkSlaveReachable::MarkSlaveReachable(const SlaveInfo& _info)
  : info(_info)
{
  CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
}


Try<bool> MarkSlaveReachable::perform(
    Registry* registry,
    hashset<SlaveID>* slaveIDs)
{
  // A slave might try to reregister that appears in the list of
  // admitted slaves. This can occur when the master fails over:
  // agents will usually attempt to reregister with the new master
  // before they are marked unreachable. In this situation, the
  // registry is already in the correct state, so no changes are
  // needed.
  if (slaveIDs->contains(info.id())) {
    return false; // No mutation.
  }

  Registry::Slave reachable;
  reachable.mutable_info()->CopyFrom(info);

  // Check whether the slave is in the unreachable list.
  // TODO(neilc): Optimize this to avoid linear scan.
  bool found = false;
  for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
    const Registry::UnreachableSlave& slave =
      registry->unreachable().slaves(i);

    if (slave.id() == info.id()) {
      // Copy the draining and deactivation states.
      if (slave.has_drain_info()) {
        reachable.mutable_drain_info()->CopyFrom(slave.drain_info());
      }

      reachable.set_deactivated(slave.deactivated());

      registry->mutable_unreachable()->mutable_slaves()->DeleteSubrange(i, 1);
      found = true;
      break;
    }
  }

  if (!found) {
    LOG(WARNING) << "Allowing UNKNOWN agent to reregister: " << info;
  }

  // Downgrade the resources such that an older master can recover from
  // the checkpointed registry state.
  CHECK_SOME(downgradeResources(&info));

  // Add the slave to the admitted list, even if we didn't find it
  // in the unreachable list. This accounts for when the slave was
  // unreachable for a long time, was GC'd from the unreachable
  // list, but then eventually reregistered.
  registry->mutable_slaves()->add_slaves()->CopyFrom(reachable);
  slaveIDs->insert(info.id());

  return true; // Mutation.
}


Prune::Prune(
    const hashset<SlaveID>& _toRemoveUnreachable,
    const hashset<SlaveID>& _toRemoveGone)
  : toRemoveUnreachable(_toRemoveUnreachable)
  , toRemoveGone(_toRemoveGone)
{}


Try<bool> Prune::perform(Registry* registry, hashset<SlaveID>* /*slaveIDs*/)
{
  // Attempt to remove the SlaveIDs in the `toRemoveXXX` from the
  // unreachable/gone list. Some SlaveIDs in `toRemoveXXX` might not appear
  // in the registry; this is possible if there was a concurrent
  // registry operation.
  //
  // TODO(neilc): This has quadratic worst-case behavior, because
  // `DeleteSubrange` for a `repeated` object takes linear time.
  bool mutate = false;

  {
    int i = 0;
    while (i < registry->unreachable().slaves().size()) {
      const Registry::UnreachableSlave& slave =
        registry->unreachable().slaves(i);

      if (toRemoveUnreachable.contains(slave.id())) {
        Registry::UnreachableSlaves* unreachable =
          registry->mutable_unreachable();

        unreachable->mutable_slaves()->DeleteSubrange(i, i+1);
        mutate = true;
        continue;
      }

      i++;
    }
  }

  {
    int i = 0;
    while (i < registry->gone().slaves().size()) {
      const Registry::GoneSlave& slave = registry->gone().slaves(i);

      if (toRemoveGone.contains(slave.id())) {
        Registry::GoneSlaves* gone = registry->mutable_gone();

        gone->mutable_slaves()->DeleteSubrange(i, i+1);
        mutate = true;
        continue;
      }

      i++;
    }
  }

  return mutate;
}


RemoveSlave::RemoveSlave(const SlaveInfo& _info)
  : info(_info)
{
  CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
}


Try<bool> RemoveSlave::perform(
    Registry* registry,
    hashset<SlaveID>* slaveIDs)
{
  for (int i = 0; i < registry->slaves().slaves().size(); i++) {
    const Registry::Slave& slave = registry->slaves().slaves(i);
    if (slave.info().id() == info.id()) {
      registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
      slaveIDs->erase(info.id());
      return true; // Mutation.
    }
  }

  // Should not happen: the master will only try to remove agents
  // that are currently admitted.
  return Error("Agent not yet admitted");
}


MarkSlaveGone::MarkSlaveGone(
    const SlaveID& _id,
    const TimeInfo& _goneTime)
  : id(_id)
  , goneTime(_goneTime)
{}


Try<bool> MarkSlaveGone::perform(Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check whether the slave is already in the gone list. As currently
  // implemented, this should not be possible: the master will not
  // try to transition an already gone slave.
  for (int i = 0; i < registry->gone().slaves().size(); i++) {
    const Registry::GoneSlave& slave = registry->gone().slaves(i);

    if (slave.id() == id) {
      return Error("Agent " + stringify(id) + " already marked as gone");
    }
  }

  // Check whether the slave is in the admitted/unreachable list.
  bool found = false;
  if (slaveIDs->contains(id)) {
    found = true;
    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      const Registry::Slave& slave = registry->slaves().slaves(i);

      if (slave.info().id() == id) {
        registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
        slaveIDs->erase(id);
        break;
      }
    }
  }

  if (!found) {
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      const Registry::UnreachableSlave& slave =
        registry->unreachable().slaves(i);

      if (slave.id() == id) {
        registry->mutable_unreachable()->mutable_slaves()->DeleteSubrange(
            i, 1);

        found = true;
        break;
      }
    }
  }

  if (found) {
    Registry::GoneSlave* gone = registry->mutable_gone()->add_slaves();

    gone->mutable_id()->CopyFrom(id);
    gone->mutable_timestamp()->CopyFrom(goneTime);

    return true; // Mutation;
  }

  // Should not happen.
  return Error("Failed to find agent " + stringify(id));
}


DrainAgent::DrainAgent(
    const SlaveID& _slaveId,
    const Option<DurationInfo>& _maxGracePeriod,
    const bool _markGone)
  : slaveId(_slaveId),
    maxGracePeriod(_maxGracePeriod),
    markGone(_markGone)
{}


Try<bool> DrainAgent::perform(Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check whether the slave is in the admitted list.
  bool found = false;
  if (slaveIDs->contains(slaveId)) {
    found = true;

    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      if (registry->slaves().slaves(i).info().id() == slaveId) {
        Registry::Slave* slave = registry->mutable_slaves()->mutable_slaves(i);

        slave->mutable_drain_info()->set_state(DRAINING);

        // Copy the DrainConfig and ensure the agent is deactivated.
        if (maxGracePeriod.isSome()) {
          slave->mutable_drain_info()->mutable_config()
            ->mutable_max_grace_period()->CopyFrom(maxGracePeriod.get());
        }

        slave->mutable_drain_info()->mutable_config()->set_mark_gone(markGone);
        slave->set_deactivated(true);
        break;
      }
    }
  }

  // If not found above, check the unreachable list.
  if (!found) {
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      if (registry->unreachable().slaves(i).id() == slaveId) {
        Registry::UnreachableSlave* slave =
          registry->mutable_unreachable()->mutable_slaves(i);

        slave->mutable_drain_info()->set_state(DRAINING);

        // Copy the DrainConfig and ensure the agent is deactivated.
        if (maxGracePeriod.isSome()) {
          slave->mutable_drain_info()->mutable_config()
            ->mutable_max_grace_period()->CopyFrom(maxGracePeriod.get());
        }

        slave->mutable_drain_info()->mutable_config()->set_mark_gone(markGone);
        slave->set_deactivated(true);
        found = true;
        break;
      }
    }
  }

  // Make sure the AGENT_DRAINING minimum capability is present or added.
  if (found) {
    protobuf::master::addMinimumCapability(
        registry->mutable_minimum_capabilities(),
        MasterInfo::Capability::AGENT_DRAINING);
  }

  return found; // Mutation if found.
}


MarkAgentDrained::MarkAgentDrained(
    const SlaveID& _slaveId)
  : slaveId(_slaveId)
{}


Try<bool> MarkAgentDrained::perform(
    Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check whether the slave is in the admitted list.
  bool found = false;
  if (slaveIDs->contains(slaveId)) {
    found = true;

    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      if (registry->slaves().slaves(i).info().id() == slaveId) {
        Registry::Slave* slave = registry->mutable_slaves()->mutable_slaves(i);

        // NOTE: This should be validated/prevented on the master side.
        if (!slave->has_drain_info() ||
            slave->drain_info().state() != DRAINING) {
          return Error("Agent " + stringify(slaveId) + " is not DRAINING");
        }

        slave->mutable_drain_info()->set_state(DRAINED);
        break;
      }
    }
  }

  // If not found above, check the unreachable list.
  if (!found) {
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      if (registry->unreachable().slaves(i).id() == slaveId) {
        Registry::UnreachableSlave* slave =
          registry->mutable_unreachable()->mutable_slaves(i);

        // NOTE: This should be validated/prevented on the master side.
        if (!slave->has_drain_info() ||
            slave->drain_info().state() != DRAINING) {
          return Error("Agent " + stringify(slaveId) + " is not DRAINING");
        }

        slave->mutable_drain_info()->set_state(DRAINED);
        found = true;
        break;
      }
    }
  }

  return found; // Mutation if found.
}


DeactivateAgent::DeactivateAgent(
    const SlaveID& _slaveId)
  : slaveId(_slaveId)
{}


Try<bool> DeactivateAgent::perform(
    Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check whether the slave is in the admitted list.
  bool found = false;
  if (slaveIDs->contains(slaveId)) {
    found = true;

    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      if (registry->slaves().slaves(i).info().id() == slaveId) {
        Registry::Slave* slave = registry->mutable_slaves()->mutable_slaves(i);

        // Set the deactivated boolean.
        slave->set_deactivated(true);
        break;
      }
    }
  }

  // If not found above, check the unreachable list.
  if (!found) {
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      if (registry->unreachable().slaves(i).id() == slaveId) {
        Registry::UnreachableSlave* slave =
          registry->mutable_unreachable()->mutable_slaves(i);

        // Set the deactivated boolean.
        slave->set_deactivated(true);
        found = true;
        break;
      }
    }
  }

  // Make sure the AGENT_DRAINING minimum capability is present or added.
  if (found) {
    protobuf::master::addMinimumCapability(
        registry->mutable_minimum_capabilities(),
        MasterInfo::Capability::AGENT_DRAINING);
  }

  return found; // Mutation if found.
}


ReactivateAgent::ReactivateAgent(
    const SlaveID& _slaveId)
  : slaveId(_slaveId)
{}


Try<bool> ReactivateAgent::perform(
    Registry* registry, hashset<SlaveID>* slaveIDs)
{
  // Check whether the slave is in the admitted list.
  bool found = false;
  bool moreThanOneDeactivated = false;

  for (int i = 0; i < registry->slaves().slaves().size(); i++) {
    if (registry->slaves().slaves(i).info().id() == slaveId) {
      Registry::Slave* slave = registry->mutable_slaves()->mutable_slaves(i);

      // Clear the draining and deactivated states.
      slave->clear_drain_info();
      slave->clear_deactivated();
      found = true;

      if (moreThanOneDeactivated) {
        break;
      }

      continue;
    }

    if (registry->slaves().slaves(i).deactivated()) {
      moreThanOneDeactivated = true;

      if (found) {
        break;
      }
    }
  }

  // Check the unreachable list too.
  if (!found || !moreThanOneDeactivated) {
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      if (registry->unreachable().slaves(i).id() == slaveId) {
        Registry::UnreachableSlave* slave =
          registry->mutable_unreachable()->mutable_slaves(i);

        // Clear the draining and deactivated states.
        slave->clear_drain_info();
        slave->clear_deactivated();
        found = true;

        if (moreThanOneDeactivated) {
          break;
        }

        continue;
      }

      if (registry->unreachable().slaves(i).deactivated()) {
        moreThanOneDeactivated = true;

        if (found) {
          break;
        }
      }
    }
  }

  // If this is the last deactivated agent,
  // remove the AGENT_DRAINING minimum capability.
  if (found && !moreThanOneDeactivated) {
    protobuf::master::removeMinimumCapability(
        registry->mutable_minimum_capabilities(),
        MasterInfo::Capability::AGENT_DRAINING);
  }

  return found; // Mutation if found.
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
