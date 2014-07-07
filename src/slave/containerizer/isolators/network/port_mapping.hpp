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

#ifndef __PORT_MAPPING_ISOLATOR_HPP__
#define __PORT_MAPPING_ISOLATOR_HPP__

#include <stdint.h>

#include <sys/types.h>

#include <string>
#include <vector>

#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/interval.hpp>
#include <stout/net.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/subcommand.hpp>

#include "linux/routing/filter/ip.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The prefix this isolator uses for the virtual ethernet devices.
extern const std::string VETH_PREFIX;

// The root directory where we bind mount all the namespace handles.
extern const std::string BIND_MOUNT_ROOT;

// Responsible for allocating ephemeral ports for the port mapping
// network isolator. This class is exposed mainly for unit testing.
class EphemeralPortsAllocator
{
public:
  EphemeralPortsAllocator(
      const IntervalSet<uint16_t>& total,
      size_t _portsPerContainer)
    : free(total),
      portsPerContainer_(_portsPerContainer) {};

  // Returns the number of ephemeral ports for each container.
  size_t portsPerContainer() const { return portsPerContainer_; }

  // Allocate an ephemeral port range for a container. The allocator
  // will automatically find one port range with the given container
  // size. Returns error if the allocation cannot be fulfilled (e.g.,
  // exhausting available ephemeral ports).
  Try<Interval<uint16_t> > allocate();

  // Mark an ephemeral port range allocated. This is used in
  // 'recover'.
  void allocate(const Interval<uint16_t>& ports);

  // Deallocate an ephemeral port range.
  void deallocate(const Interval<uint16_t>& ports);

  // Return true if the port range 'ports' is managed by the
  // allocator, regardless it has been allocated to use or not.
  bool isManaged(const Interval<uint16_t>& ports)
  {
    return (free + used).contains(ports);
  }

private:
  // Given an integer x, return the smallest integer t such that t >=
  // x and t % m == 0.
  static uint32_t nextMultipleOf(uint32_t x, uint32_t m);

  IntervalSet<uint16_t> free;
  IntervalSet<uint16_t> used;

  // The number of ephemeral ports for each container.
  size_t portsPerContainer_;
};


// For the specified ports, generate a set of port ranges each of
// which can be used by a single IP filter. In other words, each port
// range needs to satisfy the following two conditions: 1) the size of
// the range is 2^n (n=0,1,2...); 2) the begin of the range is size
// aligned (i.e., begin % size == 0). This function is exposed mainly
// for unit testing.
std::vector<routing::filter::ip::PortRange> getPortRanges(
    const IntervalSet<uint16_t>& ports);


// Provides network isolation using port mapping. Each container is
// assigned a fixed set of ports (including ephemeral ports). The
// isolator will set up filters on the host such that network traffic
// to the host will be properly redirected to the corresponding
// container depending on the destination ports. The network traffic
// from containers will also be properly relayed to the host. This
// isolator is useful when the operator wants to reuse the host IP for
// all containers running on the host (e.g., there are insufficient
// IPs).
class PortMappingIsolatorProcess : public IsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags);

  virtual ~PortMappingIsolatorProcess() {}

  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& states);

  virtual process::Future<Option<CommandInfo> > prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<Limitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  struct Info
  {
    Info(const IntervalSet<uint16_t>& _nonEphemeralPorts,
         const Interval<uint16_t>& _ephemeralPorts,
         const Option<pid_t>& _pid = None())
      : nonEphemeralPorts(_nonEphemeralPorts),
        ephemeralPorts(_ephemeralPorts),
        pid(_pid) {}

    // Non-ephemeral ports used by the container. It's possible that a
    // container does not use any non-ephemeral ports. In that case,
    // 'nonEphemeralPorts' will be empty. This variable could change
    // upon 'update'.
    IntervalSet<uint16_t> nonEphemeralPorts;

    // Each container has one and only one range of ephemeral ports.
    // It cannot have more than one ranges of ephemeral ports because
    // we need to setup the ip_local_port_range (which only accepts a
    // single interval) inside the container to restrict the ephemeral
    // ports used by the container.
    const Interval<uint16_t> ephemeralPorts;

    Option<pid_t> pid;
  };

  PortMappingIsolatorProcess(
      const Flags& _flags,
      const std::string& _eth0,
      const std::string& _lo,
      const net::MAC& _hostMAC,
      const net::IP& _hostIP,
      const size_t _hostEth0MTU,
      const net::IP& _hostDefaultGateway,
      const IntervalSet<uint16_t>& _managedNonEphemeralPorts,
      const process::Owned<EphemeralPortsAllocator>& _ephemeralPortsAllocator)
    : flags(_flags),
      eth0(_eth0),
      lo(_lo),
      hostMAC(_hostMAC),
      hostIP(_hostIP),
      hostEth0MTU(_hostEth0MTU),
      hostDefaultGateway(_hostDefaultGateway),
      managedNonEphemeralPorts(_managedNonEphemeralPorts),
      ephemeralPortsAllocator(_ephemeralPortsAllocator) {}

  // Return the scripts that will be executed in the child context.
  std::string scripts(Info* info);

  // Continuations.
  Try<Nothing> _cleanup(Info* info);
  Result<Info*> _recover(pid_t pid);

  const Flags flags;

  const std::string eth0;
  const std::string lo;
  const net::MAC hostMAC;
  const net::IP hostIP;
  const size_t hostEth0MTU;
  const net::IP hostDefaultGateway;

  // All the non-ephemeral ports managed by the slave, as passed in
  // via flags.resources.
  const IntervalSet<uint16_t> managedNonEphemeralPorts;

  process::Owned<EphemeralPortsAllocator> ephemeralPortsAllocator;

  hashmap<ContainerID, Info*> infos;

  // Recovered containers from a previous run that weren't managed by
  // the network isolator.
  hashset<ContainerID> unmanaged;
};


// Defines the subcommand for 'update' that needs to be executed by a
// subprocess to update the filters inside a container.
class PortMappingUpdate : public Subcommand
{
public:
  static const std::string NAME;

  struct Flags : public flags::FlagsBase
  {
    Flags();

    bool help;
    Option<std::string> eth0_name;
    Option<std::string> lo_name;
    Option<pid_t> pid;
    Option<JSON::Object> ports_to_add;
    Option<JSON::Object> ports_to_remove;
  };

  PortMappingUpdate() : Subcommand(NAME) {}

  Flags flags;

protected:
  virtual int execute();
  virtual flags::FlagsBase* getFlags() { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PORT_MAPPING_ISOLATOR_HPP__
