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

#ifndef __PORT_MAPPING_ISOLATOR_HPP__
#define __PORT_MAPPING_ISOLATOR_HPP__

#include <stdint.h>

#include <sys/types.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <process/metrics/metrics.hpp>
#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

#include <stout/bytes.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/interval.hpp>
#include <stout/mac.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/subcommand.hpp>

#include "linux/routing/filter/ip.hpp"

#include "linux/routing/queueing/htb.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The prefix this isolator uses for the virtual ethernet devices.
// NOTE: This constant is exposed for testing.
inline std::string PORT_MAPPING_VETH_PREFIX() { return "mesos"; }


// The root directory where we bind mount all the namespace handles.
// We choose the directory '/var/run/netns' so that we can use
// iproute2 suite (e.g., ip netns show/exec) to inspect or enter the
// network namespace. This is very useful for debugging purposes.
// NOTE: This constant is exposed for testing.
inline std::string PORT_MAPPING_BIND_MOUNT_ROOT() { return "/var/run/netns"; }

// The root directory where we keep all the namespace handle
// symlinks. This is introduced in 0.23.0.
// NOTE: This constant is exposed for testing.
inline std::string PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT()
{
  return "/var/run/mesos/netns";
}


// These names are used to identify the traffic control statistics
// output for each of the Linux Traffic Control Qdiscs we report.
constexpr char NET_ISOLATOR_BW_LIMIT[] = "bw_limit";
constexpr char NET_ISOLATOR_BLOAT_REDUCTION[] = "bloat_reduction";
constexpr char NET_ISOLATOR_INGRESS_BW_LIMIT[] = "ingress_bw_limit";
constexpr char NET_ISOLATOR_INGRESS_BLOAT_REDUCTION[] =
  "ingress_bloat_reduction";


// Default minimum egress rate of 1Mbps if egress rate limiting is
// used. This ensures there is at least some bandwidth available for
// the executor to communicate with the agent.
inline Bytes DEFAULT_MINIMUM_EGRESS_RATE_LIMIT() { return Bytes(125000); }


// Responsible for allocating ephemeral ports for the port mapping
// network isolator. This class is exposed mainly for unit testing.
class EphemeralPortsAllocator
{
public:
  EphemeralPortsAllocator(
      const IntervalSet<uint16_t>& total,
      size_t _portsPerContainer)
    : free(total),
      portsPerContainer_(_portsPerContainer) {}

  // Returns the number of ephemeral ports for each container.
  size_t portsPerContainer() const { return portsPerContainer_; }

  // Allocate an ephemeral port range for a container. The allocator
  // will automatically find one port range with the given container
  // size. Returns error if the allocation cannot be fulfilled (e.g.,
  // exhausting available ephemeral ports).
  Try<Interval<uint16_t>> allocate();

  // Mark the specified ephemeral port range as allocated.
  void allocate(const Interval<uint16_t>& ports);

  // Deallocate the specified ephemeral port range.
  void deallocate(const Interval<uint16_t>& ports);

  // Return true if the specified ephemeral port range is managed by
  // the allocator, regardless it has been allocated to use or not.
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


// Return the current egress HTB configuration for the container with
// namespace named by the executor pid on the container's interface
// eth0. This is exposed for testing.
// NOTE: This function is blocking (up to ~5 seconds).
Result<routing::queueing::htb::cls::Config> recoverHTBConfig(
    pid_t pid,
    const std::string& eth0,
    const Flags& flags);


class PercentileRatesCollector final
{
public:
  explicit PercentileRatesCollector(
      pid_t _executorPid, const Duration& _window, const Duration& _interval);

  Option<process::Statistics<uint64_t>> rxRate() const;
  Option<process::Statistics<uint64_t>> rxPacketRate() const;
  Option<process::Statistics<uint64_t>> rxDropRate() const;
  Option<process::Statistics<uint64_t>> rxErrorRate() const;

  Option<process::Statistics<uint64_t>> txRate() const;
  Option<process::Statistics<uint64_t>> txPacketRate() const;
  Option<process::Statistics<uint64_t>> txDropRate() const;
  Option<process::Statistics<uint64_t>> txErrorRate() const;

  // Register the statistics sample.
  void sample(const process::Time& ts, hashmap<std::string, uint64_t>&& stats);

  // Name of the link to sample metrics from.
  const std::string link;

private:
  void sampleRate(
      const hashmap<std::string, uint64_t>& statistics,
      const std::string& metric,
      const process::Time& timestamp,
      double timeDelta,
      process::TimeSeries<uint64_t>& rates);

  process::TimeSeries<uint64_t> rxRates;
  process::TimeSeries<uint64_t> rxPackets;
  process::TimeSeries<uint64_t> rxDrops;
  process::TimeSeries<uint64_t> rxErrors;

  process::TimeSeries<uint64_t> txRates;
  process::TimeSeries<uint64_t> txPackets;
  process::TimeSeries<uint64_t> txDrops;
  process::TimeSeries<uint64_t> txErrors;

  // Previous sample and its timestamp for calculating rates.
  Option<process::Time> previous;
  hashmap<std::string, uint64_t> previousStatistics;
};


class RatesCollector;


// Provides network isolation using port mapping. Each container is
// assigned a fixed set of ports (including ephemeral ports). The
// isolator will set up filters on the host such that network traffic
// to the host will be properly redirected to the corresponding
// container depending on the destination ports. The network traffic
// from containers will also be properly relayed to the host. This
// isolator is useful when the operator wants to reuse the host IP for
// all containers running on the host (e.g., there are insufficient
// IPs).
class PortMappingIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~PortMappingIsolatorProcess() override {}

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
  struct Info
  {
    Info(const IntervalSet<uint16_t>& _nonEphemeralPorts,
         const Interval<uint16_t>& _ephemeralPorts,
         const Option<routing::queueing::htb::cls::Config>& _egressConfig,
         const Option<routing::queueing::htb::cls::Config>& _ingressConfig,
         const Option<pid_t>& _pid = None())
      : nonEphemeralPorts(_nonEphemeralPorts),
        ephemeralPorts(_ephemeralPorts),
        egressConfig(_egressConfig),
        ingressConfig(_ingressConfig),
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

    // Optional HTB configuration for egress and ingress traffic. This
    // may change upon 'update'.
    Option<routing::queueing::htb::cls::Config> egressConfig;
    Option<routing::queueing::htb::cls::Config> ingressConfig;

    Option<pid_t> pid;
    Option<uint16_t> flowId;
  };

  // Define the metrics used by the port mapping network isolator.
  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::Counter adding_eth0_ip_filters_errors;
    process::metrics::Counter adding_eth0_ip_filters_already_exist;
    process::metrics::Counter adding_eth0_egress_filters_errors;
    process::metrics::Counter adding_eth0_egress_filters_already_exist;
    process::metrics::Counter adding_lo_ip_filters_errors;
    process::metrics::Counter adding_lo_ip_filters_already_exist;
    process::metrics::Counter adding_veth_ip_filters_errors;
    process::metrics::Counter adding_veth_ip_filters_already_exist;
    process::metrics::Counter adding_veth_icmp_filters_errors;
    process::metrics::Counter adding_veth_icmp_filters_already_exist;
    process::metrics::Counter adding_veth_arp_filters_errors;
    process::metrics::Counter adding_veth_arp_filters_already_exist;
    process::metrics::Counter adding_eth0_icmp_filters_errors;
    process::metrics::Counter adding_eth0_icmp_filters_already_exist;
    process::metrics::Counter adding_eth0_arp_filters_errors;
    process::metrics::Counter adding_eth0_arp_filters_already_exist;
    process::metrics::Counter removing_eth0_ip_filters_errors;
    process::metrics::Counter removing_eth0_ip_filters_do_not_exist;
    process::metrics::Counter removing_eth0_egress_filters_errors;
    process::metrics::Counter removing_eth0_egress_filters_do_not_exist;
    process::metrics::Counter removing_lo_ip_filters_errors;
    process::metrics::Counter removing_lo_ip_filters_do_not_exist;
    process::metrics::Counter removing_veth_ip_filters_errors;
    process::metrics::Counter removing_veth_ip_filters_do_not_exist;
    process::metrics::Counter removing_eth0_icmp_filters_errors;
    process::metrics::Counter removing_eth0_icmp_filters_do_not_exist;
    process::metrics::Counter removing_eth0_arp_filters_errors;
    process::metrics::Counter removing_eth0_arp_filters_do_not_exist;
    process::metrics::Counter updating_eth0_icmp_filters_errors;
    process::metrics::Counter updating_eth0_icmp_filters_already_exist;
    process::metrics::Counter updating_eth0_icmp_filters_do_not_exist;
    process::metrics::Counter updating_eth0_arp_filters_errors;
    process::metrics::Counter updating_eth0_arp_filters_already_exist;
    process::metrics::Counter updating_eth0_arp_filters_do_not_exist;
    process::metrics::Counter updating_container_ip_filters_errors;
    process::metrics::PushGauge per_cpu_egress_rate_limit;
    process::metrics::PushGauge per_cpu_ingress_rate_limit;
  } metrics;

  PortMappingIsolatorProcess(
      const Flags& _flags,
      const std::string& _bindMountRoot,
      const std::string& _eth0,
      const std::string& _lo,
      const net::MAC& _hostMAC,
      const net::IP::Network& _hostIPNetwork,
      const size_t _hostEth0MTU,
      const net::IP& _hostDefaultGateway,
      const routing::Handle& _hostTxFqCodelHandle,
      const hashmap<std::string, std::string>& _hostNetworkConfigurations,
      const IntervalSet<uint16_t>& _managedNonEphemeralPorts,
      const process::Owned<EphemeralPortsAllocator>& _ephemeralPortsAllocator,
      const std::set<uint16_t>& _flowIDs,
      const process::Owned<RatesCollector>& _ratesCollector,
      const Option<Bytes>& _egressRatePerCpu,
      const Option<Bytes>& _ingressRatePerCpu);

  // Continuations.
  Try<Nothing> _cleanup(Info* info, const Option<ContainerID>& containerId);
  Try<Info*> _recover(pid_t pid);

  void _update(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status);

  process::Future<ResourceStatistics> _usage(
      const ResourceStatistics& result,
      const ContainerID& containerId,
      const process::Subprocess& s);

  process::Future<ResourceStatistics> __usage(
      ResourceStatistics result,
      const ContainerID& containerId,
      const process::Future<std::string>& out);

  // Helper functions.
  Try<Nothing> addHostIPFilters(
      const routing::filter::ip::PortRange& range,
      const Option<uint16_t>& flowId,
      const std::string& veth);

  Try<Nothing> removeHostIPFilters(
      const routing::filter::ip::PortRange& range,
      const std::string& veth,
      bool removeFiltersOnVeth = true);

  // Determine the egress rate limit to apply to a container, either
  // None if no limit should be applied or some rate determined from
  // a fixed limit or a limit scaled by CPU.
  Option<routing::queueing::htb::cls::Config> egressHTBConfig(
      const Resources& resources) const;

  // Determine the ingress rate limit to apply to a container, either
  // None if no limit should be applied or some rate determined from a
  // fixed limit or a limit scaled by CPU.
  Option<routing::queueing::htb::cls::Config> ingressHTBConfig(
      const Resources& resources) const;

  // Return the scripts that will be executed in the child context.
  std::string scripts(Info* info);

  uint16_t getNextFlowId();

  const Flags flags;
  const std::string bindMountRoot;

  const std::string eth0;
  const std::string lo;
  const net::MAC hostMAC;
  const net::IP::Network hostIPNetwork;
  const size_t hostEth0MTU;
  const net::IP hostDefaultGateway;
  const routing::Handle hostTxFqCodelHandle;

  // Describe the host network configurations. It is a map between
  // configure proc files (e.g., /proc/sys/net/core/somaxconn) and
  // values of the configure proc files.
  const hashmap<std::string, std::string> hostNetworkConfigurations;

  // All the non-ephemeral ports managed by the slave, as passed in
  // via flags.resources.
  const IntervalSet<uint16_t> managedNonEphemeralPorts;

  process::Owned<EphemeralPortsAllocator> ephemeralPortsAllocator;

  // Store a set of unused flow ID's on this slave.
  std::set<uint16_t> freeFlowIds;

  hashmap<ContainerID, Info*> infos;

  // Recovered containers from a previous run that weren't managed by
  // the network isolator.
  hashset<ContainerID> unmanaged;

  process::Owned<RatesCollector> ratesCollector;

  const Option<Bytes> egressRatePerCpu;
  const Option<Bytes> ingressRatePerCpu;
};


// Defines the subcommand for 'update' that needs to be executed by a
// subprocess to update the filters inside a container.
class PortMappingUpdate : public Subcommand
{
public:
  static const char* NAME;

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<std::string> eth0_name;
    Option<std::string> lo_name;
    Option<pid_t> pid;
    Option<JSON::Object> ports_to_add;
    Option<JSON::Object> ports_to_remove;
    Option<JSON::Object> htb_config;
  };

  PortMappingUpdate() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};


// Defines the subcommand for 'statistics' that needs to be executed
// by a subprocess to retrieve newtork statistics from inside a
// container.
class PortMappingStatistics : public Subcommand
{
public:
  static const char* NAME;

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<std::string> eth0_name;
    Option<pid_t> pid;
    bool enable_socket_statistics_summary;
    bool enable_socket_statistics_details;
    bool enable_snmp_statistics;
  };

  PortMappingStatistics() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};


// Defines the subcommand for determining the egress HTB configuration
// for a container. The qdisc/class is currently on the container's
// interface inside the network namespace.
class PortMappingHTBConfig : public Subcommand
{
public:
  static const char* NAME;

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<std::string> eth0_name;
    Option<pid_t> pid;
  };

  PortMappingHTBConfig() : Subcommand(NAME) {}

  Flags flags;

protected:
  virtual int execute();
  virtual flags::FlagsBase* getFlags() { return &flags; }
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PORT_MAPPING_ISOLATOR_HPP__
