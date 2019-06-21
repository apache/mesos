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

#ifndef __MASTER_REGISTRY_OPERATIONS_HPP__
#define __MASTER_REGISTRY_OPERATIONS_HPP__

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include "master/registrar.hpp"


namespace mesos {
namespace internal {
namespace master {

// Add a new slave to the list of admitted slaves.
class AdmitSlave : public RegistryOperation
{
public:
  explicit AdmitSlave(const SlaveInfo& _info);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  SlaveInfo info;
};


// Update the SlaveInfo of an existing admitted slave.
class UpdateSlave : public RegistryOperation
{
public:
  explicit UpdateSlave(const SlaveInfo& _info);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  SlaveInfo info;
};


// Move a slave from the list of admitted slaves to the list of
// unreachable slaves.
class MarkSlaveUnreachable : public RegistryOperation
{
public:
  MarkSlaveUnreachable(
      const SlaveInfo& _info,
      const TimeInfo& _unreachableTime);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveInfo info;
  const TimeInfo unreachableTime;
};


// Add a slave back to the list of admitted slaves. The slave will
// typically be in the "unreachable" list; if so, it is removed from
// that list. The slave might also be in the "admitted" list already.
// Finally, the slave might be in neither the "unreachable" or
// "admitted" lists, if its metadata has been garbage collected from
// the registry.
class MarkSlaveReachable : public RegistryOperation
{
public:
  explicit MarkSlaveReachable(const SlaveInfo& _info);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  SlaveInfo info;
};


class Prune : public RegistryOperation
{
public:
  explicit Prune(
      const hashset<SlaveID>& _toRemoveUnreachable,
      const hashset<SlaveID>& _toRemoveGone);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* /*slaveIDs*/)
    override;

private:
  const hashset<SlaveID> toRemoveUnreachable;
  const hashset<SlaveID> toRemoveGone;
};


class RemoveSlave : public RegistryOperation
{
public:
  explicit RemoveSlave(const SlaveInfo& _info);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveInfo info;
};


// Move a slave from the list of admitted/unreachable slaves
// to the list of gone slaves.
class MarkSlaveGone : public RegistryOperation
{
public:
  MarkSlaveGone(const SlaveID& _id, const TimeInfo& _goneTime);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveID id;
  const TimeInfo goneTime;
};


// Marks an existing agent for draining.
// Also adds a minimum capability to the master for AGENT_DRAINING.
class DrainAgent : public RegistryOperation
{
public:
  DrainAgent(
      const SlaveID& _slaveId,
      const Option<DurationInfo>& _maxGracePeriod,
      const bool _markGone);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveID slaveId;
  const Option<DurationInfo> maxGracePeriod;
  const bool markGone;
};


// Transitions a DRAINING agent into the DRAINED state.
class MarkAgentDrained : public RegistryOperation
{
public:
  MarkAgentDrained(const SlaveID& _slaveId);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveID slaveId;
};


// Marks an existing agent as deactivated.
// Also adds a minimum capability to the master for AGENT_DRAINING.
class DeactivateAgent : public RegistryOperation
{
public:
  DeactivateAgent(const SlaveID& _slaveId);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveID slaveId;
};


// Clears draining or deactivation from an existing agent.
// If there are no remaining draining or deactivated agents,
// this also clears the minimum capability for AGENT_DRAINING.
class ReactivateAgent : public RegistryOperation
{
public:
  ReactivateAgent(const SlaveID& _slaveId);

protected:
  Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs) override;

private:
  const SlaveID slaveId;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_REGISTRY_OPERATIONS_HPP__
