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

#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/pid.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/mac.hpp>
#include <stout/multihashmap.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/stat.hpp>

#include "common/status_utils.hpp"
#include "common/values.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "linux/routing/route.hpp"
#include "linux/routing/utils.hpp"

#include "linux/routing/diagnosis/diagnosis.hpp"

#include "linux/routing/filter/basic.hpp"
#include "linux/routing/filter/icmp.hpp"
#include "linux/routing/filter/ip.hpp"

#include "linux/routing/handle.hpp"

#include "linux/routing/link/link.hpp"
#include "linux/routing/link/veth.hpp"

#include "linux/routing/queueing/fq_codel.hpp"
#include "linux/routing/queueing/htb.hpp"
#include "linux/routing/queueing/ingress.hpp"
#include "linux/routing/queueing/statistics.hpp"

#include "mesos/resources.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/mesos/isolators/network/port_mapping.hpp"

using namespace mesos::internal;

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;
using namespace routing::queueing::statistics;

using std::cerr;
using std::cout;
using std::dec;
using std::endl;
using std::hex;
using std::list;
using std::ostringstream;
using std::set;
using std::sort;
using std::string;
using std::vector;

using filter::ip::PortRange;

using mesos::internal::values::rangesToIntervalSet;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

// An old glibc might not have this symbol.
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

namespace mesos {
namespace internal {
namespace slave {

// The minimum number of ephemeral ports a container should have.
static const uint16_t MIN_EPHEMERAL_PORTS_SIZE = 16;

// Linux traffic control is a combination of queueing disciplines,
// filters and classes organized as a tree for the ingress (rx) and
// egress (tx) flows for each interface. Each container provides two
// networking interfaces, a virtual eth0 and a loopback interface. The
// flow of packets from the external network to container is shown
// below:
//
//   +----------------------+----------------------+
//   |                   Container                 |
//   |----------------------|----------------------|
//   |       eth0           |          lo          |
//   +----------------------+----------------------+
//          ^   |         ^           |
//      [3] |   | [4]     |           |
//          |   |     [7] +-----------+ [10]
//          |   |
//          |   |     [8] +-----------+ [9]
//      [2] |   | [5]     |           |
//          |   v         v           v
//   +----------------------+----------------------+
//   |      veth0           |          lo          |
//   +----------------------|----------------------+
//   |                     Host                    |
//   |----------------------|----------------------|
//   |                    eth0                     |
//   +----------------------+----------------------|
//                    ^           |
//                [1] |           | [6]
//                    |           v
//
// Traffic flowing from outside the network into a container enters
// the system via the host ingress interface [1] and is routed based
// on destination port to the outbound interface for the matching
// container [2], which forwards the packet to the container's inbound
// virtual interface. Outbound traffic destined for the external
// network flows along the reverse path [4,5,6]. Loopback traffic is
// directed to the corresponding Ethernet interface, either [7,10] or
// [8,9] where the same destination port routing can be applied as to
// external traffic. We use traffic control filters on several of the
// interfaces to create these packet paths.
//
// Linux provides only a very simple topology for ingress interfaces.
// A root is provided on a fixed handle (handle::INGRESS_ROOT) under
// which a single qdisc can be installed, with handle ingress::HANDLE.
// Traffic control filters can then be attached to the ingress qdisc.
// We install one or more ingress filters on the host eth0 [1] to
// direct traffic to the correct container, and on the container
// virtual eth0 [5] to direct traffic to other containers or out of
// the box. Since we know the ip port assignments for each container,
// we can direct traffic directly to the appropriate container.
// However, for ICMP and ARP traffic where no equivalent to a port
// exists, we send a copy of the packet to every container and rely on
// the network stack to drop unexpected packets.
//
// We install a Hierarchical Token Bucket (HTB) qdisc and class to
// limit the outbound traffic bandwidth as the egress qdisc inside the
// container [4] and then add a fq_codel qdisc to limit head of line
// blocking on the egress filter. The egress traffic control chain is
// thus:
//
// root device: handle::EGRESS_ROOT ->
//    htb egress qdisc: CONTAINER_TX_HTB_HANDLE ->
//        htb rate limiting class: CONTAINER_TX_HTB_CLASS_ID ->
//            buffer-bloat reduction: FQ_CODEL
constexpr Handle CONTAINER_TX_HTB_HANDLE = Handle(1, 0);
constexpr Handle CONTAINER_TX_HTB_CLASS_ID =
    Handle(CONTAINER_TX_HTB_HANDLE, 1);


// Finally we create a second fq_codel qdisc on the public interface
// of the host [6] to reduce performance interference between
// containers. We create independent flows for each container, and
// one for the host, which ensures packets from each container are
// guaranteed fair access to the host interface. This egress traffic
// control chain for the host interface is thus:
//
// root device: handle::EGRESS_ROOT ->
//    buffer-bloat reduction: FQ_CODEL
constexpr Handle HOST_TX_FQ_CODEL_HANDLE = Handle(1, 0);


// The primary priority used by each type of filter.
static const uint8_t ARP_FILTER_PRIORITY = 1;
static const uint8_t ICMP_FILTER_PRIORITY = 2;
static const uint8_t IP_FILTER_PRIORITY = 3;
static const uint8_t DEFAULT_FILTER_PRIORITY = 4;


// The secondary priorities used by filters.
static const uint8_t HIGH = 1;
static const uint8_t NORMAL = 2;
static const uint8_t LOW = 3;


// We assign a separate flow on host eth0 egress for each container
// (See MESOS-2422 for details). Host egress traffic is assigned to a
// reserved flow (HOST_FLOWID). ARP and ICMP traffic from containers
// are not heavy, so they can share the same flow.
static const uint16_t HOST_FLOWID = 1;
static const uint16_t ARP_FLOWID = 2;
static const uint16_t ICMP_FLOWID = 2;
static const uint16_t CONTAINER_MIN_FLOWID = 3;


// The well known ports. Used for sanity check.
static Interval<uint16_t> WELL_KNOWN_PORTS()
{
  return (Bound<uint16_t>::closed(0), Bound<uint16_t>::open(1024));
}


/////////////////////////////////////////////////
// Helper functions for the isolator.
/////////////////////////////////////////////////

// Given an integer x, find the largest integer t such that t <= x and
// t is aligned to power of 2.
static uint32_t roundDownToPowerOfTwo(uint32_t x)
{
  // Mutate x from 00001XXX to 0x00001111.

  // We know the MSB has to be a 1, so kill the LSB and make sure the
  // first 2 most significant bits are 1s.
  x = x | (x >> 1);

  // Now that the 2 most significant bits are 1s, make sure the first
  // 4 most significant bits are 1s, too.
  x = x | (x >> 2);

  // We keep going. Note that the 0s left to the MSB are never turned
  // to 1s.
  x = x | (x >> 4);
  x = x | (x >> 8);

  // Now we have covered all 32 bits.
  x = x | (x >> 16);

  // 0x00001111 - (0x00001111 >> 1)
  return x - (x >> 1);
}


// Returns the name of the host end of the virtual ethernet pair for a
// given container. The kernel restricts link name to 16 characters or
// less, so we cannot put container ID into the device name. Instead,
// we use the pid of the executor process forked by the slave to
// uniquely name the device for each container. It's safe because we
// cannot have two active containers having the same pid for the
// executor process.
static string veth(pid_t pid)
{
  return PORT_MAPPING_VETH_PREFIX() + stringify(pid);
}


// Extracts the pid from the given veth name.
static Option<pid_t> getPidFromVeth(const string& veth)
{
  if (strings::startsWith(veth, PORT_MAPPING_VETH_PREFIX())) {
    Try<pid_t> pid = numify<pid_t>(
        strings::remove(veth, PORT_MAPPING_VETH_PREFIX(), strings::PREFIX));

    if (pid.isSome()) {
      return pid.get();
    }
  }

  return None();
}


// Extracts the container ID from the symlink that points to the
// network namespace handle. The following is the layout of the bind
// mount root and bind mount symlink root:
//  <PORT_MAPPING_BIND_MOUNT_ROOT()>
//    |--- 3945 (pid)                           <-|
//                                                |
//  <PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT()>      |
//    |--- ecf293e7-e6e8-4cbc-aaee-4d6c958aa276 --|
//         (symlink: container ID -> pid)
static Try<ContainerID> getContainerIdFromSymlink(const string& symlink)
{
  if (!os::stat::islink(symlink)) {
    return Error("Not a symlink");
  }

  string _containerId = Path(symlink).basename();

  ContainerID containerId;
  containerId.set_value(_containerId);

  return containerId;
}


// Extracts the pid from the network namespace handle. Returns None if
// the handle is clearly not created by us.
static Result<pid_t> getPidFromNamespaceHandle(const string& handle)
{
  if (os::stat::islink(handle)) {
    return Error("Not expecting a symlink");
  }

  string _pid = Path(handle).basename();

  Try<pid_t> pid = numify<pid_t>(_pid);
  if (pid.isError()) {
    return None();
  }

  return pid.get();
}


// Extracts the pid from the symlink that points to the network
// namespace handle. Returns None if it's a dangling symlink.
static Result<pid_t> getPidFromSymlink(const string& symlink)
{
  if (!os::stat::islink(symlink)) {
    return Error("Not a symlink");
  }

  Result<string> target = os::realpath(symlink);
  if (target.isError()) {
    return Error("Failed to follow the symlink: " + target.error());
  } else if (target.isNone()) {
    // This is a dangling symlink.
    return None();
  }

  return getPidFromNamespaceHandle(target.get());
}


static string getSymlinkPath(const ContainerID& containerId)
{
  return path::join(
      PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT(),
      stringify(containerId));
}


static string getNamespaceHandlePath(const string& bindMountRoot, pid_t pid)
{
  return path::join(bindMountRoot, stringify(pid));
}

/////////////////////////////////////////////////
// Implementation for PortMappingUpdate.
/////////////////////////////////////////////////

const char* PortMappingUpdate::NAME = "update";


PortMappingUpdate::Flags::Flags()
{
  add(&Flags::eth0_name,
      "eth0_name",
      "The name of the public network interface (e.g., eth0)");

  add(&Flags::lo_name,
      "lo_name",
      "The name of the loopback network interface (e.g., lo)");

  add(&Flags::pid,
      "pid",
      "The pid of the process whose namespaces we will enter");

  add(&Flags::ports_to_add,
      "ports_to_add",
      "A collection of port ranges (formatted as a JSON object)\n"
      "for which to add IP filters. E.g.,\n"
      "--ports_to_add={\"range\":[{\"begin\":4,\"end\":8}]}");

  add(&Flags::ports_to_remove,
      "ports_to_remove",
      "A collection of port ranges (formatted as a JSON object)\n"
      "for which to remove IP filters. E.g.,\n"
      "--ports_to_remove={\"range\":[{\"begin\":4,\"end\":8}]}");
}


// The following two helper functions allow us to convert from a
// collection of port ranges to a JSON object and vice versa. They
// will be used for the port mapping update operation.
template <typename Iterable>
JSON::Object json(const Iterable& ranges)
{
  Value::Ranges values;
  foreach (const PortRange& range, ranges) {
    Value::Range value;
    value.set_begin(range.begin());
    value.set_end(range.end());

    values.add_range()->CopyFrom(value);
  }
  return JSON::protobuf(values);
}


static Try<vector<PortRange>> parse(const JSON::Object& object)
{
  Try<Value::Ranges> parsing = protobuf::parse<Value::Ranges>(object);
  if (parsing.isError()) {
    return Error("Failed to parse JSON: " + parsing.error());
  }

  vector<PortRange> ranges;
  Value::Ranges values = parsing.get();
  for (int i = 0; i < values.range_size(); i++) {
    const Value::Range& value = values.range(i);
    Try<PortRange> range = PortRange::fromBeginEnd(value.begin(), value.end());
    if (range.isError()) {
      return Error("Invalid port range: " + range.error());
    }

    ranges.push_back(range.get());
  }
  return ranges;
}


// Helper function to set up IP filters inside the container for a
// given port range.
static Try<Nothing> addContainerIPFilters(
    const PortRange& range,
    const string& eth0,
    const string& lo)
{
  // Add an IP packet filter on lo such that local traffic inside a
  // container will not be redirected to eth0.
  Try<bool> loTerminal = filter::ip::create(
      lo,
      ingress::HANDLE,
      ip::Classifier(None(), None(), None(), range),
      Priority(IP_FILTER_PRIORITY, HIGH),
      action::Terminal());

  if (loTerminal.isError()) {
    return Error(
        "Failed to create an IP packet filter on " + lo +
        " which stops packets from being sent to " + eth0 +
        ": " + loTerminal.error());
  } else if (!loTerminal.get()) {
    return Error(
        "The IP packet filter on " + lo +
        " which stops packets from being sent to " +
        eth0 + " already exists");
  }

  // Add an IP packet filter (for loopback IP) from eth0 to lo to
  // redirect all loopback IP traffic to lo.
  Try<bool> eth0ToLoLoopback = filter::ip::create(
      eth0,
      ingress::HANDLE,
      ip::Classifier(
          None(),
          net::IP::Network::LOOPBACK_V4().address(),
          None(),
          range),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(lo));

  if (eth0ToLoLoopback.isError()) {
    return Error(
        "Failed to create an IP packet filter (for loopback IP) from " +
        eth0 + " to " + lo + ": " + eth0ToLoLoopback.error());
  } else if (!eth0ToLoLoopback.get()) {
    return Error(
        "The IP packet filter (for loopback IP) from " +
        eth0 + " to " + lo + " already exists");
  }

  return Nothing();
}


// Helper function to remove IP filters inside the container for a
// given port range.
static Try<Nothing> removeContainerIPFilters(
    const PortRange& range,
    const string& eth0,
    const string& lo)
{
  // Remove the 'terminal' IP packet filter on lo.
  Try<bool> loTerminal = filter::ip::remove(
      lo,
      ingress::HANDLE,
      ip::Classifier(None(), None(), None(), range));

  if (loTerminal.isError()) {
    return Error(
        "Failed to remove the IP packet filter on " + lo +
        " which stops packets from being sent to " + eth0 +
        ": " + loTerminal.error());
  } else if (!loTerminal.get()) {
    return Error(
        "The IP packet filter on " + lo +
        " which stops packets from being sent to " + eth0 +
        " does not exist");
  }

  // Remove the IP packet filter (for loopback IP) from eth0 to lo.
  Try<bool> eth0ToLoLoopback = filter::ip::remove(
      eth0,
      ingress::HANDLE,
      ip::Classifier(
          None(),
          net::IP::Network::LOOPBACK_V4().address(),
          None(),
          range));

  if (eth0ToLoLoopback.isError()) {
    return Error(
        "Failed to remove the IP packet filter (for loopback IP) from " +
        eth0 + " to " + lo + ": " + eth0ToLoLoopback.error());
  } else if (!eth0ToLoLoopback.get()) {
    return Error(
        "The IP packet filter (for loopback IP) from " +
        eth0 + " to " + lo + " does not exist");
  }

  return Nothing();
}


int PortMappingUpdate::execute()
{
  if (flags.help) {
    cerr << "Usage: " << name() << " [OPTIONS]" << endl << endl
         << "Supported options:" << endl
         << flags.usage();
    return 0;
  }

  if (flags.eth0_name.isNone()) {
    cerr << "The public interface name (e.g., eth0) is not specified" << endl;
    return 1;
  }

  if (flags.lo_name.isNone()) {
    cerr << "The loopback interface name (e.g., lo) is not specified" << endl;
    return 1;
  }

  if (flags.pid.isNone()) {
    cerr << "The pid is not specified" << endl;
    return 1;
  }

  if (flags.ports_to_add.isNone() && flags.ports_to_remove.isNone()) {
    cerr << "Nothing to update" << endl;
    return 1;
  }

  Option<vector<PortRange>> portsToAdd;
  Option<vector<PortRange>> portsToRemove;

  if (flags.ports_to_add.isSome()) {
    Try<vector<PortRange>> parsing = parse(flags.ports_to_add.get());
    if (parsing.isError()) {
      cerr << "Parsing 'ports_to_add' failed: " << parsing.error() << endl;
      return 1;
    }
    portsToAdd = parsing.get();
  }

  if (flags.ports_to_remove.isSome()) {
    Try<vector<PortRange>> parsing = parse(flags.ports_to_remove.get());
    if (parsing.isError()) {
      cerr << "Parsing 'ports_to_remove' failed: " << parsing.error() << endl;
      return 1;
    }
    portsToRemove = parsing.get();
  }

  // Enter the network namespace.
  Try<Nothing> setns = ns::setns(flags.pid.get(), "net");
  if (setns.isError()) {
    cerr << "Failed to enter the network namespace of pid " << flags.pid.get()
         << ": " << setns.error() << endl;
    return 1;
  }

  // Update IP packet filters.
  const string eth0 = flags.eth0_name.get();
  const string lo = flags.lo_name.get();

  if (portsToAdd.isSome()) {
    foreach (const PortRange& range, portsToAdd.get()) {
      Try<Nothing> add = addContainerIPFilters(range, eth0, lo);
      if (add.isError()) {
        cerr << "Failed to add IP filters: " << add.error() << endl;
        return 1;
      }
    }
  }

  if (portsToRemove.isSome()) {
    foreach (const PortRange& range, portsToRemove.get()) {
      Try<Nothing> remove = removeContainerIPFilters(range, eth0, lo);
      if (remove.isError()) {
        cerr << "Failed to remove IP filters: " << remove.error() << endl;
        return 1;
      }
    }
  }

  return 0;
}

/////////////////////////////////////////////////
// Implementation for PortMappingStatistics.
/////////////////////////////////////////////////

const char* PortMappingStatistics::NAME = "statistics";


PortMappingStatistics::Flags::Flags()
{
  add(&Flags::eth0_name,
      "eth0_name",
      "The name of the public network interface (e.g., eth0)");

  add(&Flags::pid,
      "pid",
      "The pid of the process whose namespaces we will enter");

  add(&Flags::enable_socket_statistics_summary,
      "enable_socket_statistics_summary",
      "Whether to collect socket statistics summary for this container\n",
      false);

  add(&Flags::enable_socket_statistics_details,
      "enable_socket_statistics_details",
      "Whether to collect socket statistics details (e.g., TCP RTT)\n"
      "for this container.",
      false);

  add(&Flags::enable_snmp_statistics,
      "enable_snmp_statistics",
      "Whether to collect SNMP statistics details (e.g., TCPRetransSegs)\n"
      "for this container.",
      false);
}


// A helper that copies the traffic control statistics from the
// statistics hashmap into the ResourceStatistics protocol buffer.
static void addTrafficControlStatistics(
    const string& id,
    const hashmap<string, uint64_t>& statistics,
    ResourceStatistics* result)
{
  TrafficControlStatistics *tc = result->add_net_traffic_control_statistics();

  tc->set_id(id);

  // TODO(pbrett) Use protobuf reflection here.
  if (statistics.contains(BACKLOG)) {
    tc->set_backlog(statistics.at(BACKLOG));
  }
  if (statistics.contains(BYTES)) {
    tc->set_bytes(statistics.at(BYTES));
  }
  if (statistics.contains(DROPS)) {
    tc->set_drops(statistics.at(DROPS));
  }
  if (statistics.contains(OVERLIMITS)) {
    tc->set_overlimits(statistics.at(OVERLIMITS));
  }
  if (statistics.contains(PACKETS)) {
    tc->set_packets(statistics.at(PACKETS));
  }
  if (statistics.contains(QLEN)) {
    tc->set_qlen(statistics.at(QLEN));
  }
  if (statistics.contains(RATE_BPS)) {
    tc->set_ratebps(statistics.at(RATE_BPS));
  }
  if (statistics.contains(RATE_PPS)) {
    tc->set_ratepps(statistics.at(RATE_PPS));
  }
  if (statistics.contains(REQUEUES)) {
    tc->set_requeues(statistics.at(REQUEUES));
  }
}


static void addIPStatistics(
    const hashmap<string, int64_t>& statistics,
    ResourceStatistics* result)
{
  SNMPStatistics *snmp = result->mutable_net_snmp_statistics();
  IpStatistics *ip = snmp->mutable_ip_stats();

  // TODO(cwang): Use protobuf reflection here.
  if (statistics.contains("Forwarding")) {
    ip->set_forwarding(statistics.at("Forwarding"));
  }
  if (statistics.contains("DefaultTTL")) {
    ip->set_defaultttl(statistics.at("DefaultTTL"));
  }
  if (statistics.contains("InReceives")) {
    ip->set_inreceives(statistics.at("InReceives"));
  }
  if (statistics.contains("InHdrErrors")) {
    ip->set_inhdrerrors(statistics.at("InHdrErrors"));
  }
  if (statistics.contains("InAddrErrors")) {
    ip->set_inaddrerrors(statistics.at("InAddrErrors"));
  }
  if (statistics.contains("ForwDatagrams")) {
    ip->set_forwdatagrams(statistics.at("ForwDatagrams"));
  }
  if (statistics.contains("InUnknownProtos")) {
    ip->set_inunknownprotos(statistics.at("InUnknownProtos"));
  }
  if (statistics.contains("InDiscards")) {
    ip->set_indiscards(statistics.at("InDiscards"));
  }
  if (statistics.contains("InDelivers")) {
    ip->set_indelivers(statistics.at("InDelivers"));
  }
  if (statistics.contains("OutRequests")) {
    ip->set_outrequests(statistics.at("OutRequests"));
  }
  if (statistics.contains("OutDiscards")) {
    ip->set_outdiscards(statistics.at("OutDiscards"));
  }
  if (statistics.contains("OutNoRoutes")) {
    ip->set_outnoroutes(statistics.at("OutNoRoutes"));
  }
  if (statistics.contains("ReasmTimeout")) {
    ip->set_reasmtimeout(statistics.at("ReasmTimeout"));
  }
  if (statistics.contains("ReasmReqds")) {
    ip->set_reasmreqds(statistics.at("ReasmReqds"));
  }
  if (statistics.contains("ReasmOKs")) {
    ip->set_reasmoks(statistics.at("ReasmOKs"));
  }
  if (statistics.contains("ReasmFails")) {
    ip->set_reasmfails(statistics.at("ReasmFails"));
  }
  if (statistics.contains("FragOKs")) {
    ip->set_fragoks(statistics.at("FragOKs"));
  }
  if (statistics.contains("FragFails")) {
    ip->set_fragfails(statistics.at("FragFails"));
  }
  if (statistics.contains("FragCreates")) {
    ip->set_fragcreates(statistics.at("FragCreates"));
  }
}


static void addICMPStatistics(
    const hashmap<string, int64_t>& statistics,
    ResourceStatistics* result)
{
  SNMPStatistics *snmp = result->mutable_net_snmp_statistics();
  IcmpStatistics *icmp = snmp->mutable_icmp_stats();

  // TODO(cwang): Use protobuf reflection here.
  if (statistics.contains("InMsgs")) {
    icmp->set_inmsgs(statistics.at("InMsgs"));
  }
  if (statistics.contains("InErrors")) {
    icmp->set_inerrors(statistics.at("InErrors"));
  }
  if (statistics.contains("InCsumErrors")) {
    icmp->set_incsumerrors(statistics.at("InCsumErrors"));
  }
  if (statistics.contains("InDestUnreachs")) {
    icmp->set_indestunreachs(statistics.at("InDestUnreachs"));
  }
  if (statistics.contains("InTimeExcds")) {
    icmp->set_intimeexcds(statistics.at("InTimeExcds"));
  }
  if (statistics.contains("InParmProbs")) {
    icmp->set_inparmprobs(statistics.at("InParmProbs"));
  }
  if (statistics.contains("InSrcQuenchs")) {
    icmp->set_insrcquenchs(statistics.at("InSrcQuenchs"));
  }
  if (statistics.contains("InRedirects")) {
    icmp->set_inredirects(statistics.at("InRedirects"));
  }
  if (statistics.contains("InEchos")) {
    icmp->set_inechos(statistics.at("InEchos"));
  }
  if (statistics.contains("InEchoReps")) {
    icmp->set_inechoreps(statistics.at("InEchoReps"));
  }
  if (statistics.contains("InTimestamps")) {
    icmp->set_intimestamps(statistics.at("InTimestamps"));
  }
  if (statistics.contains("InTimestampReps")) {
    icmp->set_intimestampreps(statistics.at("InTimestampReps"));
  }
  if (statistics.contains("InAddrMasks")) {
    icmp->set_inaddrmasks(statistics.at("InAddrMasks"));
  }
  if (statistics.contains("InAddrMaskReps")) {
    icmp->set_inaddrmaskreps(statistics.at("InAddrMaskReps"));
  }
  if (statistics.contains("OutMsgs")) {
    icmp->set_outmsgs(statistics.at("OutMsgs"));
  }
  if (statistics.contains("OutErrors")) {
    icmp->set_outerrors(statistics.at("OutErrors"));
  }
  if (statistics.contains("OutDestUnreachs")) {
    icmp->set_outdestunreachs(statistics.at("OutDestUnreachs"));
  }
  if (statistics.contains("OutTimeExcds")) {
    icmp->set_outtimeexcds(statistics.at("OutTimeExcds"));
  }
  if (statistics.contains("OutParmProbs")) {
    icmp->set_outparmprobs(statistics.at("OutParmProbs"));
  }
  if (statistics.contains("OutSrcQuenchs")) {
    icmp->set_outsrcquenchs(statistics.at("OutSrcQuenchs"));
  }
  if (statistics.contains("OutRedirects")) {
    icmp->set_outredirects(statistics.at("OutRedirects"));
  }
  if (statistics.contains("OutEchos")) {
    icmp->set_outechos(statistics.at("OutEchos"));
  }
  if (statistics.contains("OutEchoReps")) {
    icmp->set_outechoreps(statistics.at("OutEchoReps"));
  }
  if (statistics.contains("OutTimestamps")) {
    icmp->set_outtimestamps(statistics.at("OutTimestamps"));
  }
  if (statistics.contains("OutTimestampReps")) {
    icmp->set_outtimestampreps(statistics.at("OutTimestampReps"));
  }
  if (statistics.contains("OutAddrMasks")) {
    icmp->set_outaddrmasks(statistics.at("OutAddrMasks"));
  }
  if (statistics.contains("OutAddrMaskReps")) {
    icmp->set_outaddrmaskreps(statistics.at("OutAddrMaskReps"));
  }
}


static void addTCPStatistics(
    const hashmap<string, int64_t>& statistics,
    ResourceStatistics* result)
{
  SNMPStatistics *snmp = result->mutable_net_snmp_statistics();
  TcpStatistics *tcp = snmp->mutable_tcp_stats();

  // TODO(cwang): Use protobuf reflection here.
  if (statistics.contains("RtoAlgorithm")) {
    tcp->set_rtoalgorithm(statistics.at("RtoAlgorithm"));
  }
  if (statistics.contains("RtoMin")) {
    tcp->set_rtomin(statistics.at("RtoMin"));
  }
  if (statistics.contains("RtoMax")) {
    tcp->set_rtomax(statistics.at("RtoMax"));
  }
  if (statistics.contains("MaxConn")) {
    tcp->set_maxconn(statistics.at("MaxConn"));
  }
  if (statistics.contains("ActiveOpens")) {
    tcp->set_activeopens(statistics.at("ActiveOpens"));
  }
  if (statistics.contains("PassiveOpens")) {
    tcp->set_passiveopens(statistics.at("PassiveOpens"));
  }
  if (statistics.contains("AttemptFails")) {
    tcp->set_attemptfails(statistics.at("AttemptFails"));
  }
  if (statistics.contains("EstabResets")) {
    tcp->set_estabresets(statistics.at("EstabResets"));
  }
  if (statistics.contains("CurrEstab")) {
    tcp->set_currestab(statistics.at("CurrEstab"));
  }
  if (statistics.contains("InSegs")) {
    tcp->set_insegs(statistics.at("InSegs"));
  }
  if (statistics.contains("OutSegs")) {
    tcp->set_outsegs(statistics.at("OutSegs"));
  }
  if (statistics.contains("RetransSegs")) {
    tcp->set_retranssegs(statistics.at("RetransSegs"));
  }
  if (statistics.contains("InErrs")) {
    tcp->set_inerrs(statistics.at("InErrs"));
  }
  if (statistics.contains("OutRsts")) {
    tcp->set_outrsts(statistics.at("OutRsts"));
  }
  if (statistics.contains("InCsumErrors")) {
    tcp->set_incsumerrors(statistics.at("InCsumErrors"));
  }
}


static void addUDPStatistics(
    const hashmap<string, int64_t>& statistics,
    ResourceStatistics* result)
{
  SNMPStatistics *snmp = result->mutable_net_snmp_statistics();
  UdpStatistics *udp = snmp->mutable_udp_stats();

  // TODO(cwang): Use protobuf reflection here.
  if (statistics.contains("InDatagrams")) {
    udp->set_indatagrams(statistics.at("InDatagrams"));
  }
  if (statistics.contains("NoPorts")) {
    udp->set_noports(statistics.at("NoPorts"));
  }
  if (statistics.contains("InErrors")) {
    udp->set_inerrors(statistics.at("InErrors"));
  }
  if (statistics.contains("OutDatagrams")) {
    udp->set_outdatagrams(statistics.at("OutDatagrams"));
  }
  if (statistics.contains("RcvbufErrors")) {
    udp->set_rcvbuferrors(statistics.at("RcvbufErrors"));
  }
  if (statistics.contains("SndbufErrors")) {
    udp->set_sndbuferrors(statistics.at("SndbufErrors"));
  }
  if (statistics.contains("InCsumErrors")) {
    udp->set_incsumerrors(statistics.at("InCsumErrors"));
  }
  if (statistics.contains("IgnoredMulti")) {
    udp->set_ignoredmulti(statistics.at("IgnoredMulti"));
  }
}


int PortMappingStatistics::execute()
{
  if (flags.help) {
    cerr << "Usage: " << name() << " [OPTIONS]" << endl << endl
         << "Supported options:" << endl
         << flags.usage();
    return 0;
  }

  if (flags.pid.isNone()) {
    cerr << "The pid is not specified" << endl;
    return 1;
  }

  if (flags.eth0_name.isNone()) {
    cerr << "The public interface name (e.g., eth0) is not specified" << endl;
    return 1;
  }

  // Enter the network namespace.
  Try<Nothing> setns = ns::setns(flags.pid.get(), "net");
  if (setns.isError()) {
    // This could happen if the executor exits before this function is
    // invoked. We do not log here to avoid spurious logging.
    return 1;
  }

  ResourceStatistics result;

  // NOTE: We use a dummy value here since this field will be cleared
  // before the result is sent to the containerizer.
  result.set_timestamp(0);

  if (flags.enable_socket_statistics_summary) {
    // Collections for socket statistics summary are below.

    // For TCP, get the number of ACTIVE and TIME_WAIT connections,
    // from reading /proc/net/sockstat (/proc/net/sockstat6 for IPV6).
    // This is not as expensive in the kernel because only counter
    // values are accessed instead of a dump of all the sockets.
    // Example output:

    // $ cat /proc/net/sockstat
    // sockets: used 1391
    // TCP: inuse 33 orphan 0 tw 0 alloc 37 mem 6
    // UDP: inuse 15 mem 7
    // UDPLITE: inuse 0
    // RAW: inuse 0
    // FRAG: inuse 0 memory 0

    Try<string> value = os::read("/proc/net/sockstat");
    if (value.isError()) {
      cerr << "Failed to read /proc/net/sockstat: " << value.error() << endl;
      return 1;
    }

    foreach (const string& line, strings::tokenize(value.get(), "\n")) {
      if (!strings::startsWith(line, "TCP")) {
        continue;
      }

      vector<string> tokens = strings::tokenize(line, " ");
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == "inuse") {
          if (i + 1 >= tokens.size()) {
            cerr << "Unexpected output from /proc/net/sockstat" << endl;
            // Be a bit forgiving here here since the /proc file
            // output format can change, though not very likely.
            continue;
          }

          // Set number of active TCP connections.
          Try<size_t> inuse = numify<size_t>(tokens[i+1]);
          if (inuse.isError()) {
            cerr << "Failed to parse the number of tcp connections in use: "
                 << inuse.error() << endl;
            continue;
          }

          result.set_net_tcp_active_connections(inuse.get());
        } else if (tokens[i] == "tw") {
          if (i + 1 >= tokens.size()) {
            cerr << "Unexpected output from /proc/net/sockstat" << endl;
            // Be a bit forgiving here here since the /proc file
            // output format can change, though not very likely.
            continue;
          }

          // Set number of TIME_WAIT TCP connections.
          Try<size_t> tw = numify<size_t>(tokens[i+1]);
          if (tw.isError()) {
            cerr << "Failed to parse the number of tcp connections in"
                 << " TIME_WAIT: " << tw.error() << endl;
            continue;
          }

          result.set_net_tcp_time_wait_connections(tw.get());
        }
      }
    }
  }

  if (flags.enable_socket_statistics_details) {
    // Collections for socket statistics details are below.

    // NOTE: If the underlying library uses the older version of
    // kernel API, the family argument passed in may not be honored.
    Try<vector<diagnosis::socket::Info>> infos =
      diagnosis::socket::infos(AF_INET, diagnosis::socket::state::ALL);

    if (infos.isError()) {
      cerr << "Failed to retrieve the socket information" << endl;
      return 1;
    }

    vector<uint32_t> RTTs;
    foreach (const diagnosis::socket::Info& info, infos.get()) {
      // We double check on family regardless.
      if (info.family != AF_INET) {
        continue;
      }

      // We consider all sockets that have non-zero rtt value.
      if (info.tcpInfo.isSome() && info.tcpInfo->tcpi_rtt != 0) {
        RTTs.push_back(info.tcpInfo->tcpi_rtt);
      }
    }

    // Only print to stdout when we have results.
    if (RTTs.size() > 0) {
      std::sort(RTTs.begin(), RTTs.end());

      // NOTE: The size of RTTs is usually within 1 million so we
      // don't need to worry about overflow here.
      // TODO(jieyu): Right now, we choose to use "Nearest rank" for
      // simplicity. Consider directly using the Statistics abstraction
      // which computes "Linear interpolation between closest ranks".
      // http://en.wikipedia.org/wiki/Percentile
      size_t p50 = RTTs.size() * 50 / 100;
      size_t p90 = RTTs.size() * 90 / 100;
      size_t p95 = RTTs.size() * 95 / 100;
      size_t p99 = RTTs.size() * 99 / 100;

      result.set_net_tcp_rtt_microsecs_p50(RTTs[p50]);
      result.set_net_tcp_rtt_microsecs_p90(RTTs[p90]);
      result.set_net_tcp_rtt_microsecs_p95(RTTs[p95]);
      result.set_net_tcp_rtt_microsecs_p99(RTTs[p99]);
    }
  }

  if (flags.enable_snmp_statistics) {
    Try<string> value = os::read("/proc/net/snmp");
    if (value.isError()) {
      cerr << "Failed to read /proc/net/snmp: " << value.error() << endl;
      return 1;
    }

    hashmap<string, hashmap<string, int64_t>> SNMPStats;
    vector<string> keys;
    bool isKeyLine = true;
    foreach (const string& line, strings::tokenize(value.get(), "\n")) {
      vector<string> fields = strings::tokenize(line, ":");
      if (fields.size() != 2) {
        cerr << "Failed to tokenize line '" << line << "' "
             << " in /proc/net/snmp" << endl;
        return 1;
      }
      vector<string> tokens = strings::tokenize(fields[1], " ");
      if (isKeyLine) {
        for (size_t i = 0; i < tokens.size(); i++) {
          keys.push_back(tokens[i]);
        }
      } else {
        hashmap<string, int64_t> stats;
        for (size_t i = 0; i < tokens.size(); i++) {
          Try<int64_t> val = numify<int64_t>(tokens[i]);

          if (val.isError()) {
            cerr << "Failed to parse the statistics in " << fields[0]
                 << val.error() << endl;
            return 1;
          }
          stats[keys[i]] = val.get();
        }
        SNMPStats[fields[0]] = stats;
        keys.clear();
      }
      isKeyLine = !isKeyLine;
    }

    addIPStatistics(SNMPStats["Ip"], &result);
    addICMPStatistics(SNMPStats["Icmp"], &result);
    addTCPStatistics(SNMPStats["Tcp"], &result);
    addUDPStatistics(SNMPStats["Udp"], &result);
  }

  // Collect traffic statistics for the container from the container
  // virtual interface and export them in JSON.
  const string& eth0 = flags.eth0_name.get();

  // Overlimits are reported on the HTB qdisc at the egress root.
  Result<hashmap<string, uint64_t>> statistics =
    htb::statistics(eth0, EGRESS_ROOT);

  if (statistics.isSome()) {
    addTrafficControlStatistics(
        NET_ISOLATOR_BW_LIMIT,
        statistics.get(),
        &result);
  } else if (statistics.isNone()) {
    // Traffic control statistics are only available when the
    // container is created on a slave when the egress rate limit is
    // on (i.e., egress_rate_limit_per_container flag is set). We
    // can't just test for that flag here however, since the slave may
    // have been restarted with different flags since the container
    // was created. It is also possible that isolator statistics are
    // unavailable because we the container is in the process of being
    // created or destroy. Hence we do not report a lack of network
    // statistics as an error.
  } else if (statistics.isError()) {
    cerr << "Failed to get htb qdisc statistics on " << eth0
         << " in namespace " << flags.pid.get() << endl;
  }

  // Drops due to the bandwidth limit should be reported at the leaf.
  statistics = fq_codel::statistics(eth0, CONTAINER_TX_HTB_CLASS_ID);
  if (statistics.isSome()) {
    addTrafficControlStatistics(
        NET_ISOLATOR_BLOAT_REDUCTION,
        statistics.get(),
        &result);
  } else if (statistics.isNone()) {
    // See discussion on network isolator statistics above.
  } else if (statistics.isError()) {
    cerr << "Failed to get fq_codel qdisc statistics on " << eth0
         << " in namespace " << flags.pid.get() << endl;
  }

  cout << stringify(JSON::protobuf(result));
  return 0;
}


/////////////////////////////////////////////////
// Implementation for the isolator.
/////////////////////////////////////////////////

PortMappingIsolatorProcess::Metrics::Metrics()
  : adding_eth0_ip_filters_errors(
        "port_mapping/adding_eth0_ip_filters_errors"),
    adding_eth0_ip_filters_already_exist(
        "port_mapping/adding_eth0_ip_filters_already_exist"),
    adding_eth0_egress_filters_errors(
        "port_mapping/adding_eth0_egress_filters_errors"),
    adding_eth0_egress_filters_already_exist(
        "port_mapping/adding_eth0_egress_filters_already_exist"),
    adding_lo_ip_filters_errors(
        "port_mapping/adding_lo_ip_filters_errors"),
    adding_lo_ip_filters_already_exist(
        "port_mapping/adding_lo_ip_filters_already_exist"),
    adding_veth_ip_filters_errors(
        "port_mapping/adding_veth_ip_filters_errors"),
    adding_veth_ip_filters_already_exist(
        "port_mapping/adding_veth_ip_filters_already_exist"),
    adding_veth_icmp_filters_errors(
        "port_mapping/adding_veth_icmp_filters_errors"),
    adding_veth_icmp_filters_already_exist(
        "port_mapping/adding_veth_icmp_filters_already_exist"),
    adding_veth_arp_filters_errors(
        "port_mapping/adding_veth_arp_filters_errors"),
    adding_veth_arp_filters_already_exist(
        "port_mapping/adding_veth_arp_filters_already_exist"),
    adding_eth0_icmp_filters_errors(
        "port_mapping/adding_eth0_icmp_filters_errors"),
    adding_eth0_icmp_filters_already_exist(
        "port_mapping/adding_eth0_icmp_filters_already_exist"),
    adding_eth0_arp_filters_errors(
        "port_mapping/adding_eth0_arp_filters_errors"),
    adding_eth0_arp_filters_already_exist(
        "port_mapping/adding_eth0_arp_filters_already_exist"),
    removing_eth0_ip_filters_errors(
        "port_mapping/removing_eth0_ip_filters_errors"),
    removing_eth0_ip_filters_do_not_exist(
        "port_mapping/removing_eth0_ip_filters_do_not_exist"),
    removing_eth0_egress_filters_errors(
        "port_mapping/removing_eth0_egress_filters_errors"),
    removing_eth0_egress_filters_do_not_exist(
        "port_mapping/removinging_eth0_egress_filters_do_not_exist"),
    removing_lo_ip_filters_errors(
        "port_mapping/removing_lo_ip_filters_errors"),
    removing_lo_ip_filters_do_not_exist(
        "port_mapping/removing_lo_ip_filters_do_not_exist"),
    removing_veth_ip_filters_errors(
        "port_mapping/removing_veth_ip_filters_errors"),
    removing_veth_ip_filters_do_not_exist(
        "port_mapping/removing_veth_ip_filters_do_not_exist"),
    removing_eth0_icmp_filters_errors(
        "port_mapping/removing_eth0_icmp_filters_errors"),
    removing_eth0_icmp_filters_do_not_exist(
        "port_mapping/removing_eth0_icmp_filters_do_not_exist"),
    removing_eth0_arp_filters_errors(
        "port_mapping/removing_eth0_arp_filters_errors"),
    removing_eth0_arp_filters_do_not_exist(
        "port_mapping/removing_eth0_arp_filters_do_not_exist"),
    updating_eth0_icmp_filters_errors(
        "port_mapping/updating_eth0_icmp_filters_errors"),
    updating_eth0_icmp_filters_already_exist(
        "port_mapping/updating_eth0_icmp_filters_already_exist"),
    updating_eth0_icmp_filters_do_not_exist(
        "port_mapping/updating_eth0_icmp_filters_do_not_exist"),
    updating_eth0_arp_filters_errors(
        "port_mapping/updating_eth0_arp_filters_errors"),
    updating_eth0_arp_filters_already_exist(
        "port_mapping/updating_eth0_arp_filters_already_exist"),
    updating_eth0_arp_filters_do_not_exist(
        "port_mapping/updating_eth0_arp_filters_do_not_exist"),
    updating_container_ip_filters_errors(
        "port_mapping/updating_container_ip_filters_errors")
{
  process::metrics::add(adding_eth0_ip_filters_errors);
  process::metrics::add(adding_eth0_ip_filters_already_exist);
  process::metrics::add(adding_lo_ip_filters_errors);
  process::metrics::add(adding_lo_ip_filters_already_exist);
  process::metrics::add(adding_veth_ip_filters_errors);
  process::metrics::add(adding_veth_ip_filters_already_exist);
  process::metrics::add(adding_veth_icmp_filters_errors);
  process::metrics::add(adding_veth_icmp_filters_already_exist);
  process::metrics::add(adding_veth_arp_filters_errors);
  process::metrics::add(adding_veth_arp_filters_already_exist);
  process::metrics::add(adding_eth0_icmp_filters_errors);
  process::metrics::add(adding_eth0_icmp_filters_already_exist);
  process::metrics::add(adding_eth0_arp_filters_errors);
  process::metrics::add(adding_eth0_arp_filters_already_exist);
  process::metrics::add(removing_eth0_ip_filters_errors);
  process::metrics::add(removing_eth0_ip_filters_do_not_exist);
  process::metrics::add(removing_lo_ip_filters_errors);
  process::metrics::add(removing_lo_ip_filters_do_not_exist);
  process::metrics::add(removing_veth_ip_filters_errors);
  process::metrics::add(removing_veth_ip_filters_do_not_exist);
  process::metrics::add(removing_eth0_icmp_filters_errors);
  process::metrics::add(removing_eth0_icmp_filters_do_not_exist);
  process::metrics::add(removing_eth0_arp_filters_errors);
  process::metrics::add(removing_eth0_arp_filters_do_not_exist);
  process::metrics::add(updating_eth0_icmp_filters_errors);
  process::metrics::add(updating_eth0_icmp_filters_already_exist);
  process::metrics::add(updating_eth0_icmp_filters_do_not_exist);
  process::metrics::add(updating_eth0_arp_filters_errors);
  process::metrics::add(updating_eth0_arp_filters_already_exist);
  process::metrics::add(updating_eth0_arp_filters_do_not_exist);
  process::metrics::add(updating_container_ip_filters_errors);
}


PortMappingIsolatorProcess::Metrics::~Metrics()
{
  process::metrics::remove(adding_eth0_ip_filters_errors);
  process::metrics::remove(adding_eth0_ip_filters_already_exist);
  process::metrics::remove(adding_lo_ip_filters_errors);
  process::metrics::remove(adding_lo_ip_filters_already_exist);
  process::metrics::remove(adding_veth_ip_filters_errors);
  process::metrics::remove(adding_veth_ip_filters_already_exist);
  process::metrics::remove(adding_veth_icmp_filters_errors);
  process::metrics::remove(adding_veth_icmp_filters_already_exist);
  process::metrics::remove(adding_veth_arp_filters_errors);
  process::metrics::remove(adding_veth_arp_filters_already_exist);
  process::metrics::remove(adding_eth0_icmp_filters_errors);
  process::metrics::remove(adding_eth0_icmp_filters_already_exist);
  process::metrics::remove(adding_eth0_arp_filters_errors);
  process::metrics::remove(adding_eth0_arp_filters_already_exist);
  process::metrics::remove(removing_eth0_ip_filters_errors);
  process::metrics::remove(removing_eth0_ip_filters_do_not_exist);
  process::metrics::remove(removing_lo_ip_filters_errors);
  process::metrics::remove(removing_lo_ip_filters_do_not_exist);
  process::metrics::remove(removing_veth_ip_filters_errors);
  process::metrics::remove(removing_veth_ip_filters_do_not_exist);
  process::metrics::remove(removing_eth0_icmp_filters_errors);
  process::metrics::remove(removing_eth0_icmp_filters_do_not_exist);
  process::metrics::remove(removing_eth0_arp_filters_errors);
  process::metrics::remove(removing_eth0_arp_filters_do_not_exist);
  process::metrics::remove(updating_eth0_icmp_filters_errors);
  process::metrics::remove(updating_eth0_icmp_filters_already_exist);
  process::metrics::remove(updating_eth0_icmp_filters_do_not_exist);
  process::metrics::remove(updating_eth0_arp_filters_errors);
  process::metrics::remove(updating_eth0_arp_filters_already_exist);
  process::metrics::remove(updating_eth0_arp_filters_do_not_exist);
  process::metrics::remove(updating_container_ip_filters_errors);
}


Try<Isolator*> PortMappingIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("Using network isolator requires root permissions");
  }

  // Verify that the network namespace is available by checking the
  // existence of the network namespace handle of the current process.
  Try<bool> netSupported = ns::supported(CLONE_NEWNET);
  if (netSupported.isError() || !netSupported.get()) {
    return Error(
        "Using network isolator requires network namespace. "
        "Make sure your kernel is newer than 3.4");
  }

  // Check the routing library.
  Try<Nothing> check = routing::check();
  if (check.isError()) {
    return Error(
        "Routing library check failed: " +
        check.error());
  }

  // Check the availability of a few Linux commands that we will use.
  // We use the blocking os::shell here because 'create' will only be
  // invoked during initialization.
  Try<string> checkCommandTc = os::shell("tc filter show");
  if (checkCommandTc.isError()) {
    return Error("Check command 'tc' failed: " + checkCommandTc.error());
  }

  // NOTE: loopback device always exists.
  Try<string> checkCommandEthtool = os::shell("ethtool -k lo");
  if (checkCommandEthtool.isError()) {
    return Error("Check command 'ethtool' failed: "
                 + checkCommandEthtool.error());
  }

  Try<string> checkCommandIp = os::shell("ip link show");
  if (checkCommandIp.isError()) {
    return Error("Check command 'ip' failed: " + checkCommandIp.error());
  }

  Try<Resources> resources = Resources::parse(
      flags.resources.getOrElse(""),
      flags.default_role);

  if (resources.isError()) {
    return Error("Failed to parse --resources: " + resources.error());
  }

  // Get 'ports' resource from 'resources' flag. These ports will be
  // treated as non-ephemeral ports.
  IntervalSet<uint16_t> nonEphemeralPorts;
  if (resources->ports().isSome()) {
    Try<IntervalSet<uint16_t>> ports = rangesToIntervalSet<uint16_t>(
        resources->ports().get());

    if (ports.isError()) {
      return Error(
          "Invalid ports resource '" +
          stringify(resources->ports().get()) +
          "': " + ports.error());
    }

    nonEphemeralPorts = ports.get();
  }

  // Get 'ephemeral_ports' resource from 'resources' flag. These ports
  // will be allocated to each container as ephemeral ports.
  IntervalSet<uint16_t> ephemeralPorts;
  if (resources->ephemeral_ports().isSome()) {
    Try<IntervalSet<uint16_t>> ports = rangesToIntervalSet<uint16_t>(
        resources->ephemeral_ports().get());

    if (ports.isError()) {
      return Error(
          "Invalid ephemeral ports resource '" +
          stringify(resources->ephemeral_ports().get()) +
          "': " + ports.error());
    }

    ephemeralPorts = ports.get();
  }

  // Each container requires at least one ephemeral port for slave
  // executor communication. If no 'ephemeral_ports' resource is
  // found, we will return error.
  if (ephemeralPorts.empty()) {
    return Error("Ephemeral ports are not specified");
  }

  // Sanity check to make sure that the ephemeral ports specified do
  // not intersect with the specified non-ephemeral ports.
  if (ephemeralPorts.intersects(nonEphemeralPorts)) {
    return Error(
        "The specified ephemeral ports " + stringify(ephemeralPorts) +
        " intersect with the specified non-ephemeral ports " +
        stringify(nonEphemeralPorts));
  }

  // This is a sanity check to make sure that the ephemeral ports
  // specified do not intersect with the well known ports.
  if (ephemeralPorts.intersects(WELL_KNOWN_PORTS())) {
    return Error(
        "The specified ephemeral ports " + stringify(ephemeralPorts) +
        " intersect with well known ports " + stringify(WELL_KNOWN_PORTS()));
  }

  // Obtain the host ephemeral port range by reading the proc file
  // system ('ip_local_port_range').
  Try<string> value = os::read("/proc/sys/net/ipv4/ip_local_port_range");
  if (value.isError()) {
    return Error("Failed to read host ip_local_port_range: " + value.error());
  }

  vector<string> split = strings::split(strings::trim(value.get()), "\t");
  if (split.size() != 2) {
    return Error(
        "Unexpected format from host ip_local_port_range: " + value.get());
  }

  Try<uint16_t> begin = numify<uint16_t>(split[0]);
  if (begin.isError()) {
    return Error(
        "Failed to parse the begin of host ip_local_port_range: " + split[0]);
  }

  Try<uint16_t> end = numify<uint16_t>(split[1]);
  if (end.isError()) {
    return Error(
        "Failed to parse the end of host ip_local_port_range: " + split[1]);
  }

  Interval<uint16_t> hostEphemeralPorts =
    (Bound<uint16_t>::closed(begin.get()),
     Bound<uint16_t>::closed(end.get()));

  // Sanity check to make sure the specified ephemeral ports do not
  // intersect with the ephemeral ports used by the host.
  if (ephemeralPorts.intersects(hostEphemeralPorts)) {
    return Error(
        "The specified ephemeral ports " + stringify(ephemeralPorts) +
        " intersect with the ephemeral ports used by the host " +
        stringify(hostEphemeralPorts));
  }

  // TODO(chzhcn): Cross check ephemeral ports with used ports on the
  // host (e.g., using port scan).

  // Initialize the ephemeral ports allocator.

  // In theory, any positive integer can be broken up into a few
  // numbers that are power of 2 aligned. We choose to not allow this
  // for now so that each container has a fixed (one) number of
  // filters for ephemeral ports. This makes it easy to debug and
  // infer performance.
  if (roundDownToPowerOfTwo(flags.ephemeral_ports_per_container) !=
      flags.ephemeral_ports_per_container) {
    return Error(
        "The number of ephemeral ports for each container (" +
        stringify(flags.ephemeral_ports_per_container) +
        ") is not a power of 2");
  }

  if (ephemeralPorts.size() < flags.ephemeral_ports_per_container) {
    return Error(
        "Network Isolator is given ephemeral ports of size: " +
        stringify(ephemeralPorts.size()) + ", but asked to allocate " +
        stringify(flags.ephemeral_ports_per_container) +
        " ephemeral ports for a container");
  }

  if (flags.ephemeral_ports_per_container < MIN_EPHEMERAL_PORTS_SIZE) {
    return Error(
        "Each container has only " +
        stringify(flags.ephemeral_ports_per_container) +
        " ephemeral ports. The minimum required is: " +
        stringify(MIN_EPHEMERAL_PORTS_SIZE));
  }

  Owned<EphemeralPortsAllocator> ephemeralPortsAllocator(
      new EphemeralPortsAllocator(
        ephemeralPorts,
        flags.ephemeral_ports_per_container));

  // Get the name of the public interface (e.g., eth0). If it is not
  // specified, try to derive its name from the routing library.
  Result<string> eth0 = link::eth0();
  if (flags.eth0_name.isSome()) {
    eth0 = flags.eth0_name.get();

    // Check if the given public interface exists.
    Try<bool> hostEth0Exists = link::exists(eth0.get());
    if (hostEth0Exists.isError()) {
      return Error(
          "Failed to check if " + eth0.get() + " exists: " +
          hostEth0Exists.error());
    } else if (!hostEth0Exists.get()) {
      return Error("The public interface " + eth0.get() + " does not exist");
    }
  } else if (!eth0.isSome()){
    // eth0 is not specified in the flag and we did not get a valid
    // eth0 from the library.
    return Error(
        "Network Isolator failed to find a public interface: " +
        (eth0.isError() ? eth0.error() : "does not have a public interface"));
  }

  LOG(INFO) << "Using " << eth0.get() << " as the public interface";

  // Get the name of the loopback interface. If it is not specified,
  // try to derive its name based on the loopback IP address.
  Result<string> lo = link::lo();
  // Option<string> lo = flags.lo_name;
  if (flags.lo_name.isSome()) {
    lo = flags.lo_name;

    // Check if the given loopback interface exists.
    Try<bool> hostLoExists = link::exists(lo.get());
    if (hostLoExists.isError()) {
      return Error(
          "Failed to check if " + lo.get() + " exists: " +
          hostLoExists.error());
    } else if (!hostLoExists.get()) {
      return Error("The loopback interface " + lo.get() + " does not exist");
    }
  } else if (!lo.isSome()) {
    // lo is not specified in the flag and we did not get a valid
    // lo from the library.
    return Error(
        "Network Isolator failed to find a loopback interface: " +
        (lo.isError() ? lo.error() : "does not have a loopback interface"));
  }

  LOG(INFO) << "Using " << lo.get() << " as the loopback interface";

  // If egress rate limit is provided, do a sanity check that it is
  // not greater than the host physical link speed.
  Option<Bytes> egressRateLimitPerContainer;
  if (flags.egress_rate_limit_per_container.isSome()) {
    // Read host physical link speed from /sys/class/net/eth0/speed.
    // This value is in MBits/s. Some distribution does not support
    // reading speed (depending on the driver). If that's the case,
    // simply print warnings.
    const string eth0SpeedPath =
      path::join("/sys/class/net", eth0.get(), "speed");

    if (!os::exists(eth0SpeedPath)) {
      LOG(WARNING) << "Cannot determine link speed of " << eth0.get()
                   << ": '" << eth0SpeedPath << "' does not exist";
    } else {
      Try<string> value = os::read(eth0SpeedPath);
      if (value.isError()) {
        // NOTE: Even if the speed file exists, the read might fail if
        // the driver does not support reading the speed. Therefore,
        // we print a warning here, instead of failing.
        LOG(WARNING) << "Cannot determine link speed of " << eth0.get()
                     << ": Failed to read '" << eth0SpeedPath
                     << "': " << value.error();
      } else {
        Try<uint64_t> hostLinkSpeed =
          numify<uint64_t>(strings::trim(value.get()));

        CHECK_SOME(hostLinkSpeed);

        // It could be possible that the nic driver doesn't support
        // reporting physical link speed. In that case, report error.
        if (hostLinkSpeed.get() == 0xFFFFFFFF) {
          LOG(WARNING) << "Link speed reporting is not supported for '"
                       << eth0.get() + "'";
        } else {
          // Convert host link speed to Bytes/s for comparason.
          if (hostLinkSpeed.get() * 1000000 / 8 <
              flags.egress_rate_limit_per_container->bytes()) {
            return Error(
                "The given egress traffic limit for containers " +
                stringify(flags.egress_rate_limit_per_container->bytes()) +
                " Bytes/s is greater than the host link speed " +
                stringify(hostLinkSpeed.get() * 1000000 / 8) + " Bytes/s");
          }
        }
      }
    }

    if (flags.egress_rate_limit_per_container.get() != Bytes(0)) {
      egressRateLimitPerContainer = flags.egress_rate_limit_per_container.get();
    } else {
      LOG(WARNING) << "Ignoring the given zero egress rate limit";
    }
  }

  // Get the host IP network, MAC and default gateway.
  Result<net::IP::Network> hostIPNetwork =
    net::IP::Network::fromLinkDevice(eth0.get(), AF_INET);

  if (!hostIPNetwork.isSome()) {
    return Error(
        "Failed to get the public IP network of " + eth0.get() + ": " +
        (hostIPNetwork.isError() ?
            hostIPNetwork.error() :
            "does not have an IPv4 network"));
  }

  Result<net::MAC> hostMAC = net::mac(eth0.get());
  if (!hostMAC.isSome()) {
    return Error(
        "Failed to get the MAC address of " + eth0.get() + ": " +
        (hostMAC.isError() ? hostMAC.error() : "does not have a MAC address"));
  }

  Result<net::IP> hostDefaultGateway = route::defaultGateway();
  if (!hostDefaultGateway.isSome()) {
    return Error(
        "Failed to get the default gateway of the host: " +
        (hostDefaultGateway.isError() ? hostDefaultGateway.error()
        : "The default gateway of the host does not exist"));
  }

  // Set the MAC address of the host loopback interface (lo) so that
  // it matches that of the host public interface (eth0).  A fairly
  // recent kernel patch is needed for this operation to succeed:
  // https://git.kernel.org/cgit/linux/kernel/git/davem/net.git/:
  // 25f929fbff0d1bcebf2e92656d33025cd330cbf8
  Try<bool> setHostLoMAC = link::setMAC(lo.get(), hostMAC.get());
  if (setHostLoMAC.isError()) {
    return Error(
        "Failed to set the MAC address of " + lo.get() +
        ": " + setHostLoMAC.error());
  }

  // Set the MTU of the host loopback interface (lo) so that it
  // matches that of the host public interface (eth0).
  Result<unsigned int> hostEth0MTU = link::mtu(eth0.get());
  if (hostEth0MTU.isError()) {
    return Error(
        "Failed to get the MTU of " + eth0.get() +
        ": " + hostEth0MTU.error());
  }

  // The host public interface should exist since we just checked it.
  CHECK_SOME(hostEth0MTU);

  Try<bool> setHostLoMTU = link::setMTU(lo.get(), hostEth0MTU.get());
  if (setHostLoMTU.isError()) {
    return Error(
        "Failed to set the MTU of " + lo.get() +
        ": " + setHostLoMTU.error());
  }

  // Prepare the ingress queueing disciplines on host public interface
  // (eth0) and host loopback interface (lo).
  Try<bool> createHostEth0IngressQdisc = ingress::create(eth0.get());
  if (createHostEth0IngressQdisc.isError()) {
    return Error(
        "Failed to create the ingress qdisc on " + eth0.get() +
        ": " + createHostEth0IngressQdisc.error());
  }

  set<uint16_t> freeFlowIds;
  Handle hostTxFqCodelHandle = HOST_TX_FQ_CODEL_HANDLE;

  if (flags.egress_unique_flow_per_container) {
    Try<Handle> egressParentHandle = Handle::parse(
        flags.egress_flow_classifier_parent);

    if (egressParentHandle.isError()) {
      return Error(
          "Failed to parse egress_flow_classifier_parent: " +
          egressParentHandle.error());
    }

    if (egressParentHandle.get() != EGRESS_ROOT) {
      // TODO(cwang): This is just a guess, we do not know if this
      // handle is available or not.
      hostTxFqCodelHandle = Handle(egressParentHandle->primary() + 1, 0);
    }

    // Prepare a fq_codel queueing discipline on host public interface
    // (eth0) for egress flow classification.
    Try<bool> existHostEth0EgressFqCodel =
      fq_codel::exists(eth0.get(), egressParentHandle.get());

    if (existHostEth0EgressFqCodel.isError()) {
      return Error(
          "Failed to check egress qdisc existence on " + eth0.get() +
          ": " + existHostEth0EgressFqCodel.error());
    }

    if (existHostEth0EgressFqCodel.get()) {
      // TODO(cwang): We don't know if this existed fq_codel qdisc is
      // created by ourselves or someone else.
      LOG(INFO) << "Using an existed egress qdisc on " << eth0.get();
    } else {
      // Either there is no qdisc at all, or there is some non-fq_codel
      // qdisc at root. We try to create one to check which is true.
      Try<bool> createHostEth0EgressQdisc = fq_codel::create(
          eth0.get(),
          egressParentHandle.get(),
          hostTxFqCodelHandle);

      if (createHostEth0EgressQdisc.isError()) {
        return Error(
            "Failed to create the egress qdisc on " + eth0.get() +
            ": " + createHostEth0EgressQdisc.error());
      } else if (!createHostEth0EgressQdisc.get()) {
        // TODO(cwang): Maybe we can continue when some other egress
        // qdisc exists because this is not a necessary qdisc for
        // network isolation, but we don't want inconsistency, so we
        // just fail in this case. See details in MESOS-2370.
        return Error("Egress qdisc already exists on " + eth0.get());
      }
    }

    // TODO(cwang): Make sure DEFAULT_FLOWS is large enough so that
    // it's unlikely to run out of free flow IDs.
    for (uint16_t i = CONTAINER_MIN_FLOWID; i < fq_codel::DEFAULT_FLOWS; i++) {
      freeFlowIds.insert(i);
    }
  }

  Try<bool> createHostLoQdisc = ingress::create(lo.get());
  if (createHostLoQdisc.isError()) {
    return Error(
        "Failed to create the ingress qdisc on " + lo.get() +
        ": " + createHostLoQdisc.error());
  }

  // Enable 'route_localnet' on host loopback interface (lo). This
  // enables the use of 127.0.0.1/8 for local routing purpose. This
  // feature only exists on kernel 3.6 or newer.
  const string loRouteLocalnet =
    path::join("/proc/sys/net/ipv4/conf", lo.get(), "route_localnet");

  if (!os::exists(loRouteLocalnet)) {
    // TODO(jieyu): Consider supporting running the isolator if this
    // feature is not available. We need to conditionally disable
    // routing for 127.0.0.1/8, and ask the tasks to use the public IP
    // for container to container and container to host communication.
    return Error("The kernel does not support 'route_localnet'");
  }

  Try<Nothing> write = os::write(loRouteLocalnet, "1");
  if (write.isError()) {
    return Error(
        "Failed to enable route_localnet for " + lo.get() +
        ": " + write.error());
  }

  // We disable 'rp_filter' and 'send_redirects' for host loopback
  // interface (lo) to work around a kernel bug, which was only
  // recently addressed in upstream in the following 3 commits.
  // https://git.kernel.org/cgit/linux/kernel/git/davem/net.git/:
  //   6a662719c9868b3d6c7d26b3a085f0cd3cc15e64
  //   0d5edc68739f1c1e0519acbea1d3f0c1882a15d7
  //   e374c618b1465f0292047a9f4c244bd71ab5f1f0
  // The workaround ensures packets don't get dropped at lo.
  write = os::write("/proc/sys/net/ipv4/conf/all/rp_filter", "0");
  if (write.isError()) {
    return Error(
        "Failed to disable rp_filter for all: " + write.error());
  }

  write = os::write(path::join(
      "/proc/sys/net/ipv4/conf", lo.get(), "rp_filter"), "0");
  if (write.isError()) {
    return Error(
        "Failed to disable rp_filter for " + lo.get() +
        ": " + write.error());
  }

  write = os::write("/proc/sys/net/ipv4/conf/all/send_redirects", "0");
  if (write.isError()) {
    return Error(
        "Failed to disable send_redirects for all: " + write.error());
  }

  write = os::write(path::join(
      "/proc/sys/net/ipv4/conf", lo.get(), "send_redirects"), "0");
  if (write.isError()) {
    return Error(
        "Failed to disable send_redirects for " + lo.get() +
        ": " + write.error());
  }

  // We need to enable accept_local on host loopback interface (lo)
  // for kernels older than 3.6. Refer to the following:
  // https://git.kernel.org/cgit/linux/kernel/git/davem/net.git/:
  //   7a9bc9b81a5bc6e44ebc80ef781332e4385083f2
  // https://www.kernel.org/doc/Documentation/networking/ip-sysctl.txt
  write = os::write(path::join(
      "/proc/sys/net/ipv4/conf", lo.get(), "accept_local"), "1");
  if (write.isError()) {
    return Error(
        "Failed to enable accept_local for " + lo.get() +
        ": " + write.error());
  }

  // Reading host network configurations. Each container will match
  // these configurations.
  hashset<string> procs;

  // TODO(jieyu): The following is a partial list of all the
  // configurations. In the future, we may want to expose these
  // configurations using ContainerInfo.

  // The kernel will use a default value for the following
  // configurations inside a container. Therefore, we need to set them
  // in the container to match that on the host.
  procs.insert("/proc/sys/net/core/somaxconn");

  // As of kernel 3.10, the following configurations are shared
  // between host and containers, and therefore are not required to be
  // set in containers. We keep them here just in case the kernel
  // changes in the future.
  procs.insert("/proc/sys/net/core/netdev_max_backlog");
  procs.insert("/proc/sys/net/core/rmem_max");
  procs.insert("/proc/sys/net/core/wmem_max");
  procs.insert("/proc/sys/net/ipv4/tcp_keepalive_time");
  procs.insert("/proc/sys/net/ipv4/tcp_keepalive_intvl");
  procs.insert("/proc/sys/net/ipv4/tcp_keepalive_probes");
  procs.insert("/proc/sys/net/ipv4/tcp_max_syn_backlog");
  procs.insert("/proc/sys/net/ipv4/tcp_rmem");
  procs.insert("/proc/sys/net/ipv4/tcp_retries2");
  procs.insert("/proc/sys/net/ipv4/tcp_synack_retries");
  procs.insert("/proc/sys/net/ipv4/tcp_wmem");
  procs.insert("/proc/sys/net/ipv4/neigh/default/gc_thresh1");
  procs.insert("/proc/sys/net/ipv4/neigh/default/gc_thresh2");
  procs.insert("/proc/sys/net/ipv4/neigh/default/gc_thresh3");

  hashmap<string, string> hostNetworkConfigurations;
  foreach (const string& proc, procs) {
    Try<string> value = os::read(proc);
    if (value.isSome()) {
      LOG(INFO) << proc << " = '" << strings::trim(value.get()) << "'";
      hostNetworkConfigurations[proc] = strings::trim(value.get());
    }
  }

  // Self bind mount PORT_MAPPING_BIND_MOUNT_ROOT(). Since we use a
  // new mount namespace for each container, for this mount point, we
  // set '--make-rshared' on the host and set '--make-rslave' inside
  // each container. This is important because when we unmount the
  // network namespace handles on the host, those handles will be
  // unmounted in the containers as well, but NOT vice versa.
  const string portMappingBindMountRoot = PORT_MAPPING_BIND_MOUNT_ROOT();

  // We create the bind mount directory if it does not exist.
  Try<Nothing> mkdir = os::mkdir(portMappingBindMountRoot);
  if (mkdir.isError()) {
    return Error(
        "Failed to create the bind mount root directory at '" +
        portMappingBindMountRoot + "': " + mkdir.error());
  }

  // We need to get the realpath for the bind mount root since on some
  // Linux distribution, The bind mount root (i.e., /var/run/netns)
  // might contain symlink.
  Result<string> bindMountRoot = os::realpath(portMappingBindMountRoot);
  if (!bindMountRoot.isSome()) {
    return Error(
        "Failed to get realpath for bind mount root '" +
        PORT_MAPPING_BIND_MOUNT_ROOT() + "': " +
        (bindMountRoot.isError() ? bindMountRoot.error() : "Not found"));
  }

  // Now, check '/proc/self/mounts' to see if the bind mount root has
  // already been self mounted.
  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
  if (mountTable.isError()) {
    return Error("Failed to read the mount table: " + mountTable.error());
  }

  Option<fs::MountInfoTable::Entry> bindMountEntry;
  foreach (const fs::MountInfoTable::Entry& entry, mountTable->entries) {
    if (entry.target == bindMountRoot.get()) {
      bindMountEntry = entry;
    }
  }

  // Do a self bind mount if needed. If the mount already exists, make
  // sure it is a shared mount of its own peer group.
  if (bindMountEntry.isNone()) {
    // NOTE: Instead of using fs::mount to perform the bind mount, we
    // use the shell command here because the syscall 'mount' does not
    // update the mount table (i.e., /etc/mtab). In other words, the
    // mount will not be visible if the operator types command
    // 'mount'. Since this mount will still be presented after all
    // containers and the slave are stopped, it's better to make it
    // visible. It's OK to use the blocking os::shell here because
    // 'create' will only be invoked during initialization.
    Try<string> mount = os::shell(
        "mount --bind %s %s && "
        "mount --make-slave %s && "
        "mount --make-shared %s",
        bindMountRoot->c_str(),
        bindMountRoot->c_str(),
        bindMountRoot->c_str(),
        bindMountRoot->c_str());

    if (mount.isError()) {
      return Error(
          "Failed to self bind mount '" + bindMountRoot.get() +
          "' and make it a shared mount: " + mount.error());
    }
  } else {
    if (bindMountEntry->shared().isNone()) {
      // This is the case where the work directory mount is not a
      // shared mount yet (possibly due to slave crash while preparing
      // the work directory mount). It's safe to re-do the following.
      Try<string> mount = os::shell(
          "mount --make-slave %s && "
          "mount --make-shared %s",
          bindMountRoot->c_str(),
          bindMountRoot->c_str());

      if (mount.isError()) {
        return Error(
            "Failed to self bind mount '" + bindMountRoot.get() +
            "' and make it a shared mount: " + mount.error());
      }
    } else {
      // We need to make sure that the shared mount is in its own peer
      // group. To check that, we need to get the parent mount.
      foreach (const fs::MountInfoTable::Entry& entry, mountTable->entries) {
        if (entry.id == bindMountEntry->parent) {
          // If the bind mount root and its parent mount are in the
          // same peer group, we need to re-do the following commands
          // so that they are in different peer groups.
          if (entry.shared() == bindMountEntry->shared()) {
            Try<string> mount = os::shell(
                "mount --make-slave %s && "
                "mount --make-shared %s",
                bindMountRoot->c_str(),
                bindMountRoot->c_str());

            if (mount.isError()) {
              return Error(
                  "Failed to self bind mount '" + bindMountRoot.get() +
                  "' and make it a shared mount: " + mount.error());
            }
          }

          break;
        }
      }
    }
  }

  // Create the network namespace handle symlink directory if it does
  // not exist. It is used to host from network namespace handle
  // symlinks whose basename is a container ID. This allows us to
  // recover container IDs for orphan containers (i.e., not known by
  // the slave). This is introduced in 0.23.0.
  mkdir = os::mkdir(PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT());
  if (mkdir.isError()) {
    return Error(
        "Failed to create the bind mount root directory at " +
        PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT() + ": " + mkdir.error());
  }

  return new MesosIsolator(Owned<MesosIsolatorProcess>(
      new PortMappingIsolatorProcess(
          flags,
          bindMountRoot.get(),
          eth0.get(),
          lo.get(),
          hostMAC.get(),
          hostIPNetwork.get(),
          hostEth0MTU.get(),
          hostDefaultGateway.get(),
          hostTxFqCodelHandle,
          hostNetworkConfigurations,
          egressRateLimitPerContainer,
          nonEphemeralPorts,
          ephemeralPortsAllocator,
          freeFlowIds)));
}


Future<Nothing> PortMappingIsolatorProcess::recover(
    const vector<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  // Extract pids from virtual device names (veth). This tells us
  // about all the potential live containers on this slave.
  Try<set<string>> links = net::links();
  if (links.isError()) {
    return Failure("Failed to get all the links: " + links.error());
  }

  hashset<pid_t> pids;
  foreach (const string& name, links.get()) {
    Option<pid_t> pid = getPidFromVeth(name);
    // Not all links follow the naming: mesos{pid}, so we simply
    // continue, e.g., eth0.
    if (pid.isNone()) {
      continue;
    } else if (pids.contains(pid.get())) {
      return Failure("Two virtual devices have the same name '" + name + "'");
    }

    pids.insert(pid.get());
  }

  // Scan the bind mount root to cleanup all stale network namespace
  // handles that do not have an active veth associated with.
  Try<list<string>> entries = os::ls(bindMountRoot);
  if (entries.isError()) {
    return Failure(
        "Failed to list bind mount root '" + bindMountRoot +
        "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    const string path = path::join(bindMountRoot, entry);

    // NOTE: We expect all regular files whose names are numbers under
    // the bind mount root are network namespace handles.
    Result<pid_t> pid = getPidFromNamespaceHandle(path);
    if (pid.isError()) {
      return Failure(
          "Failed to get pid from network namespace handle '" +
          path + "': " + pid.error());
    } else if (pid.isNone()) {
      // We ignore files that are clearly not network namespace
      // handles created by us. It's likely that those are created by
      // users or other tools.
      LOG(WARNING) << "Unrecognized network namespace handle '" << path << "'";
      continue;
    }

    // We cleanup the network namespace handle if the associated
    // containers have clearly exited (i.e., the veth has gone). The
    // cleanup here is best effort.
    if (!pids.contains(pid.get())) {
      LOG(INFO) << "Removing stale network namespace handle '" << path << "'";

      Try<Nothing> unmount = fs::unmount(path, MNT_DETACH);
      if (unmount.isError()) {
        LOG(WARNING) << "Failed to unmount stale network namespace handle '"
                     << path << "': " << unmount.error();
      }

      Try<Nothing> rm = os::rm(path);
      if (rm.isError()) {
        LOG(WARNING) << "Failed to remove stale network namespace handle '"
                     << path << "': " << rm.error();
      }
    }
  }

  // Scan the bind mount symlink root for container IDs. This allows us
  // to recover container IDs for orphan containers (i.e., not known
  // by the slave). This is introduced in 0.23.0.
  entries = os::ls(PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT());
  if (entries.isError()) {
    return Failure(
        "Failed to list bind mount symlink root '" +
        PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT() +
        "': " + entries.error());
  }

  // This map stores the mapping between pids and container IDs
  // recovered from the bind mount root that have valid veth links. We
  // use a multihashmap here because multiple container IDs can map to
  // the same pid if the removal of a symlink fails in '_cleanup()'
  // and the pid is reused by a new container.
  multihashmap<pid_t, ContainerID> linkers;

  foreach (const string& entry, entries.get()) {
    const string path =
      path::join(PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT(), entry);

    // We only create symlinks in this directory and assume
    // non-symlink files are created by other users or tools,
    // therefore will be ignored.
    if (!os::stat::islink(path)) {
      LOG(WARNING) << "Ignored non-symlink file '" << path
                   << "' under bind mount symlink root '"
                   << PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT() << "'";
      continue;
    }

    // NOTE: We expect all symlinks under the bind mount symlink root
    // to be container ID symlinks.

    Try<ContainerID> containerId = getContainerIdFromSymlink(path);
    if (containerId.isError()) {
      return Failure(
          "Failed to get container ID from network namespace handle symlink '" +
          path + "': " + containerId.error());
    }

    Result<pid_t> pid = getPidFromSymlink(path);
    if (pid.isError()) {
      return Failure(
          "Failed to get pid from network namespace handle symlink '" + path +
          "': " + pid.error());
    }

    // We remove the symlink if it's dangling or the associated
    // containers have clearly exited (i.e., the veth has gone). The
    // cleanup here is best effort.
    if (pid.isNone() || !pids.contains(pid.get())) {
      LOG(INFO) << "Removing stale network namespace handle symlink '"
                << path << "'";

      Try<Nothing> rm = os::rm(path);
      if (rm.isError()) {
        LOG(WARNING) << "Failed to remove stale network namespace handle "
                     << " symlink '" << path << "': " << rm.error();
      }
    } else {
      LOG(INFO) << "Discovered network namespace handle symlink "
                << containerId.get() << " -> " << pid.get();

      linkers.put(pid.get(), containerId.get());
    }
  }

  // If multiple container IDs point to the same pid, we remove both
  // symlinks for safety (as if we cannot derive the container ID for
  // orphans, which is OK because it'll be treated the same as those
  // containers that are created by older (pre 0.23.0) versions). Note
  // that it's possible that multiple container IDs map to the same
  // pid if the removal of a symlink fails in '_cleanup()' and the pid
  // is reused by a new container.
  foreach (pid_t pid, linkers.keys()) {
    list<ContainerID> containerIds = linkers.get(pid);
    if (containerIds.size() > 1) {
      foreach (const ContainerID& containerId, containerIds) {
        const string linker = getSymlinkPath(containerId);

        LOG(WARNING) << "Removing duplicated network namespace handle symlink '"
                     << linker << "'";

        Try<Nothing> rm = os::rm(linker);
        if (rm.isError()) {
          LOG(WARNING) << "Failed to remove duplicated network namespace "
                       << "handle symlink '" << linker << "': " << rm.error();
        }
      }

      linkers.remove(pid);
    }
  }

  // Now, actually recover the isolator from slave's state.
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    pid_t pid = state.pid();

    VLOG(1) << "Recovering network isolator for container "
            << containerId << " with pid " << pid;

    if (!pids.contains(pid)) {
      // There are two possible cases here:
      //
      // 1) The container was launched by the slave with network
      //    isolation disabled, so the pid could not be found in the
      //    device names in the system.
      //
      // 2) The container was launched by the slave with network
      //    isolation enabled, but veth is removed (because the
      //    corresponding container is destroyed), but the slave
      //    restarts before it is able to write the sentinel file.
      //
      // In both cases, we treat the container as unmanaged. For case
      // (2), it's safe to do so because the container has already
      // been destroyed.
      VLOG(1) << "Skipped recovery for container " << containerId
              << "with pid " << pid << " as either it was not managed by "
              << "the network isolator or it has already been destroyed";

      unmanaged.insert(containerId);
      continue;
    }

    Try<Info*> recover = _recover(pid);
    if (recover.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }

      return Failure(
          "Failed to recover container " + stringify(containerId) +
          " with pid " + stringify(pid) + ": " + recover.error());
    }

    infos[containerId] = recover.get();

    // Remove the successfully recovered pid.
    pids.erase(pid);
  }

  // Recover orphans. Known orphans will be destroyed by containerizer
  // using the normal cleanup path (refer to MESOS-2367 for details).
  // Unknown orphans will be cleaned up immediately. The recovery will
  // fail if there is some unknown orphan that cannot be cleaned up.
  vector<Info*> unknownOrphans;

  foreach (pid_t pid, pids) {
    Try<Info*> recover = _recover(pid);
    if (recover.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      foreach (Info* info, unknownOrphans) {
        delete info;
      }

      return Failure(
          "Failed to recover orphaned container with pid " +
          stringify(pid) + ": " + recover.error());
    }

    if (linkers.get(pid).size() == 1) {
      const ContainerID containerId = linkers.get(pid).front();
      CHECK(!infos.contains(containerId));

      if (orphans.contains(containerId)) {
        infos[containerId] = recover.get();
        continue;
      }
    }

    unknownOrphans.push_back(recover.get());
  }

  foreach (Info* info, unknownOrphans) {
    CHECK_SOME(info->pid);
    pid_t pid = info->pid.get();

    Option<ContainerID> containerId;
    if (linkers.get(pid).size() == 1) {
      containerId = linkers.get(pid).front();
    }

    // NOTE: If 'infos' is empty (means there is no regular container
    // or known orphan), the '_cleanup' below will remove the ICMP and
    // ARP packet filters on host eth0. This will cause subsequent
    // calls to '_cleanup' for unknown orphans to fail. However, this
    // is OK because when slave restarts and tries to recover again,
    // it'll try to remove the remaining unknown orphans.
    // TODO(jieyu): Consider call '_cleanup' for all the unknown
    // orphans before returning even if error occurs.
    Try<Nothing> cleanup = _cleanup(info, containerId);
    if (cleanup.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }

      // TODO(jieyu): Also delete 'info' in unknownOrphans. Notice
      // that some 'info' in unknownOrphans might have already been
      // deleted in '_cleanup' above.

      return Failure(
          "Failed to cleanup orphaned container with pid " +
          stringify(pid) + ": " + cleanup.error());
    }
  }

  // TODO(cwang): Consider removing unrecognized flow classifiers from
  // host eth0 egress.

  LOG(INFO) << "Network isolator recovery complete";

  return Nothing();
}


Try<PortMappingIsolatorProcess::Info*>
PortMappingIsolatorProcess::_recover(pid_t pid)
{
  // Get all the IP filters on veth.
  // NOTE: We only look at veth devices to recover port ranges
  // assigned to each container. That's the reason why we need to make
  // sure that we add filters to veth before adding filters to host
  // eth0 and host lo. Also, we need to make sure we remove filters
  // from host eth0 and host lo before removing filters from veth.
  Result<vector<ip::Classifier>> vethIngressClassifiers =
    ip::classifiers(veth(pid), ingress::HANDLE);

  if (vethIngressClassifiers.isError()) {
    return Error(
        "Failed to get all the IP filters on " + veth(pid) +
        ": " + vethIngressClassifiers.error());
  } else if (vethIngressClassifiers.isNone()) {
    return Error(
        "Failed to get all the IP filters on " + veth(pid) +
        ": link does not exist");
  }

  hashmap<PortRange, uint16_t> flowIds;

  if (flags.egress_unique_flow_per_container) {
    // Get all egress IP flow classifiers on eth0.
    Result<vector<filter::Filter<ip::Classifier>>> eth0EgressFilters =
      ip::filters(eth0, hostTxFqCodelHandle);

    if (eth0EgressFilters.isError()) {
      return Error(
          "Failed to get all the IP flow classifiers on " + eth0 +
          ": " + eth0EgressFilters.error());
    } else if (eth0EgressFilters.isNone()) {
      return Error(
          "Failed to get all the IP flow classifiers on " + eth0 +
          ": link does not exist");
    }

    // Construct a port range to flow ID mapping from host eth0
    // egress. This map will be used later.
    foreach (const filter::Filter<ip::Classifier>& filter,
             eth0EgressFilters.get()) {
      const Option<PortRange> sourcePorts = filter.classifier.sourcePorts;
      const Option<Handle> classid = filter.classid;

      if (sourcePorts.isNone()) {
        return Error("Missing source ports for filters on egress of " + eth0);
      }

      if (classid.isNone()) {
        return Error("Missing classid for filters on egress of " + eth0);
      }

      if (flowIds.contains(sourcePorts.get())) {
        return Error(
          "Duplicated port range " + stringify(sourcePorts.get()) +
          " detected on egress of " + eth0);
      }

      flowIds[sourcePorts.get()] = classid->secondary();
    }
  }

  IntervalSet<uint16_t> nonEphemeralPorts;
  IntervalSet<uint16_t> ephemeralPorts;
  Option<uint16_t> flowId;

  foreach (const ip::Classifier& classifier, vethIngressClassifiers.get()) {
    const Option<PortRange> sourcePorts = classifier.sourcePorts;
    const Option<PortRange> destinationPorts = classifier.destinationPorts;

    // All the IP filters on veth used by us only have source ports.
    if (sourcePorts.isNone() || destinationPorts.isSome()) {
      return Error("Unexpected IP filter detected on " + veth(pid));
    }

    if (flowIds.contains(sourcePorts.get())) {
      if (flowId.isNone()) {
        flowId = flowIds.get(sourcePorts.get());
      } else if (flowId != flowIds.get(sourcePorts.get())) {
        return Error(
            "A container is associated with multiple flows "
            "on egress of " + eth0);
      }
    } else if (flowId.isSome()) {
      // This is the case where some port range of a container is
      // assigned to a flow while some isn't. This could happen if
      // slave crashes while those filters are created. However, this
      // is OK for us because packets by default go to the host flow.
      LOG(WARNING) << "Container port range " << sourcePorts.get()
                   << " does not have flow id " << flowId.get()
                   << " assigned";
    }

    Interval<uint16_t> ports =
      (Bound<uint16_t>::closed(sourcePorts->begin()),
       Bound<uint16_t>::closed(sourcePorts->end()));

    if (managedNonEphemeralPorts.contains(ports)) {
      nonEphemeralPorts += ports;
    } else if (ephemeralPortsAllocator->isManaged(ports)) {
      // We have duplicate here because we have two IP filters with
      // the same ephemeral port range (one for eth0 and one for lo).
      // But we should never have two intersecting port ranges.
      if (!ephemeralPorts.contains(ports) && ephemeralPorts.intersects(ports)) {
        return Error("Unexpected intersected ephemeral port ranges");
      }

      ephemeralPorts += ports;
    } else {
      return Error("Unexpected IP filter detected on " + veth(pid));
    }
  }

  Info* info = nullptr;

  if (ephemeralPorts.empty()) {
    // NOTE: This is possible because the slave may crash while
    // calling 'isolate()', leaving a partially isolated container. To
    // clean up this partially isolated container, we still create an
    // Info struct here and let the 'cleanup' function clean it up
    // later.
    LOG(WARNING) << "No ephemeral ports found for container with pid "
                 << stringify(pid) << ". This could happen if agent crashes "
                 << "while isolating a container";

    info = new Info(nonEphemeralPorts, Interval<uint16_t>(), pid);
  } else {
    if (ephemeralPorts.intervalCount() != 1) {
      return Error("Each container should have only one ephemeral port range");
    }

    // Tell the allocator that this ephemeral port range is used.
    ephemeralPortsAllocator->allocate(*ephemeralPorts.begin());

    info = new Info(nonEphemeralPorts, *ephemeralPorts.begin(), pid);

    VLOG(1) << "Recovered network isolator for container with pid " << pid
            << " non-ephemeral port ranges " << nonEphemeralPorts
            << " and ephemeral port range " << *ephemeralPorts.begin();
  }

  if (flowId.isSome()) {
    freeFlowIds.erase(flowId.get());
    info->flowId = flowId.get();
  }

  return CHECK_NOTNULL(info);
}


Future<Option<ContainerLaunchInfo>> PortMappingIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (unmanaged.contains(containerId)) {
    return Failure("Asked to prepare an unmanaged container");
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  const ExecutorInfo& executorInfo = containerConfig.executor_info();
  const Resources resources(containerConfig.resources());

  IntervalSet<uint16_t> nonEphemeralPorts;

  if (resources.ports().isSome()) {
    nonEphemeralPorts = rangesToIntervalSet<uint16_t>(
        resources.ports().get()).get();

    // Sanity check to make sure that the assigned non-ephemeral ports
    // for the container are part of the non-ephemeral ports specified
    // by the slave.
    if (!managedNonEphemeralPorts.contains(nonEphemeralPorts)) {
        return Failure(
            "Some non-ephemeral ports specified in " +
            stringify(nonEphemeralPorts) +
            " are not managed by the agent");
    }
  }

  // TODO(jieyu): For now, we simply ignore the 'ephemeral_ports'
  // specified in the executor info. However, this behavior needs to
  // be changed once the master can make default allocations for
  // ephemeral ports.
  if (resources.ephemeral_ports().isSome()) {
    LOG(WARNING) << "Ignoring the specified ephemeral_ports '"
                 << resources.ephemeral_ports().get()
                 << "' for container " << containerId
                 << " of executor '" << executorInfo.executor_id() << "'";
  }

  // Allocate the ephemeral ports used by this container.
  Try<Interval<uint16_t>> ephemeralPorts = ephemeralPortsAllocator->allocate();
  if (ephemeralPorts.isError()) {
    return Failure(
        "Failed to allocate ephemeral ports: " + ephemeralPorts.error());
  }

  infos[containerId] = new Info(nonEphemeralPorts, ephemeralPorts.get());

  LOG(INFO) << "Using non-ephemeral ports " << nonEphemeralPorts
            << " and ephemeral ports " << ephemeralPorts.get()
            << " for container " << containerId << " of executor '"
            << executorInfo.executor_id() << "'";

  ContainerLaunchInfo launchInfo;
  launchInfo.add_pre_exec_commands()->set_value(scripts(infos[containerId]));

  // NOTE: the port mapping isolator itself doesn't require mount
  // namespace. However, if mount namespace is enabled because of
  // other isolators, we need to set mount sharing accordingly for
  // PORT_MAPPING_BIND_MOUNT_ROOT to avoid races described in
  // MESOS-1558. So we turn on mount namespace here for consistency.
  launchInfo.add_clone_namespaces(CLONE_NEWNET);
  launchInfo.add_clone_namespaces(CLONE_NEWNS);

  return launchInfo;
}


Future<Nothing> PortMappingIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (unmanaged.contains(containerId)) {
    return Failure("Asked to isolate an unmanaged container");
  }

  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (info->pid.isSome()) {
    return Failure("The container has already been isolated");
  }

  info->pid = pid;

  if (flags.egress_unique_flow_per_container) {
    info->flowId = getNextFlowId();
  }

  // Bind mount the network namespace handle of the process 'pid' to a
  // directory to hold an extra reference to the network namespace
  // which will be released in 'cleanup'. By holding the extra
  // reference, the network namespace will not be destroyed even if
  // the process 'pid' is gone, which allows us to explicitly control
  // the network namespace life cycle.
  const string source = path::join("/proc", stringify(pid), "ns", "net");
  const string target = getNamespaceHandlePath(bindMountRoot, pid);

  Try<Nothing> touch = os::touch(target);
  if (touch.isError()) {
    return Failure("Failed to create the bind mount point: " + touch.error());
  }

  Try<Nothing> mount = fs::mount(source, target, None(), MS_BIND, nullptr);
  if (mount.isError()) {
    return Failure(
        "Failed to mount the network namespace handle from '" +
        source + "' to '" + target + "': " + mount.error());
  }

  LOG(INFO) << "Bind mounted '" << source << "' to '" << target
            << "' for container " << containerId;

  // Since 0.23.0, we create a symlink to the network namespace handle
  // using the container ID. This serves two purposes. First, it
  // allows us to recover the container ID later when slave restarts
  // even if slave's checkpointed meta data is deleted. Second, it
  // makes the debugging easier. See MESOS-2528 for details.
  const string linker = getSymlinkPath(containerId);
  Try<Nothing> symlink = ::fs::symlink(target, linker);
  if (symlink.isError()) {
    return Failure(
        "Failed to symlink the network namespace handle '" +
        linker + "' -> '" + target + "': " + symlink.error());
  }

  LOG(INFO) << "Created network namespace handle symlink '"
            << linker << "' -> '" << target << "'";

  // Create a virtual ethernet pair for this container.
  Try<bool> createVethPair = link::veth::create(veth(pid), eth0, pid);
  if (createVethPair.isError()) {
    return Failure(
        "Failed to create virtual ethernet pair: " +
        createVethPair.error());
  }

  // We cannot reuse the existing veth pair, because one of them is
  // still inside another container.
  if (!createVethPair.get()) {
    return Failure(
        "Virtual ethernet pair " + veth(pid) + " already exists");
  }

  // Disable IPv6 for veth as IPv6 packets won't be forwarded anyway.
  const string disableIPv6 =
    path::join("/proc/sys/net/ipv6/conf", veth(pid), "disable_ipv6");

  if (os::exists(disableIPv6)) {
    Try<Nothing> write = os::write(disableIPv6, "1");
    if (write.isError()) {
      return Failure(
          "Failed to disable IPv6 for " + veth(pid) +
          ": " + write.error());
    }
  }

  // Sets the MAC address of veth to match the MAC address of the host
  // public interface (eth0).
  Try<bool> setVethMAC = link::setMAC(veth(pid), hostMAC);
  if (setVethMAC.isError()) {
    return Failure(
        "Failed to set the MAC address of " + veth(pid) +
        ": " + setVethMAC.error());
  }

  // Prepare the ingress queueing disciplines on veth.
  Try<bool> createQdisc = ingress::create(veth(pid));
  if (createQdisc.isError()) {
    return Failure(
        "Failed to create the ingress qdisc on " + veth(pid) +
        ": " + createQdisc.error());
  }

  // Veth device should exist since we just created it.
  CHECK(createQdisc.get());

  // For each port range, add a set of IP packet filters to properly
  // redirect IP traffic to/from containers.
  foreach (const PortRange& range,
           getPortRanges(info->nonEphemeralPorts + info->ephemeralPorts)) {
    if (info->flowId.isSome()) {
      LOG(INFO) << "Adding IP packet filters with ports " << range
                << " with flow ID " << info->flowId.get()
                << " for container " << containerId;
    } else {
      LOG(INFO) << "Adding IP packet filters with ports " << range
                << " for container " << containerId;
    }

    Try<Nothing> add = addHostIPFilters(range, info->flowId, veth(pid));
    if (add.isError()) {
      return Failure(
          "Failed to add IP packet filter with ports " +
          stringify(range) + " for container with pid " +
          stringify(pid) + ": " + add.error());
    }
  }

  // Relay ICMP packets from veth of the container to host eth0.
  Try<bool> icmpVethToEth0 = filter::icmp::create(
      veth(pid),
      ingress::HANDLE,
      icmp::Classifier(None()),
      Priority(ICMP_FILTER_PRIORITY, NORMAL),
      action::Redirect(eth0));

  if (icmpVethToEth0.isError()) {
    ++metrics.adding_veth_icmp_filters_errors;

    return Failure(
        "Failed to create an ICMP packet filter from " + veth(pid) +
        " to host " + eth0 + ": " + icmpVethToEth0.error());
  } else if (!icmpVethToEth0.get()) {
    ++metrics.adding_veth_icmp_filters_already_exist;

    return Failure(
        "The ICMP packet filter from " + veth(pid) +
        " to host " + eth0 + " already exists");
  }

  // Relay ARP packets from veth of the container to host eth0.
  Try<bool> arpVethToEth0 = filter::basic::create(
      veth(pid),
      ingress::HANDLE,
      ETH_P_ARP,
      Priority(ARP_FILTER_PRIORITY, NORMAL),
      action::Redirect(eth0));

  if (arpVethToEth0.isError()) {
    ++metrics.adding_veth_arp_filters_errors;

    return Failure(
        "Failed to create an ARP packet filter from " + veth(pid) +
        " to host " + eth0 + ": " + arpVethToEth0.error());
  } else if (!arpVethToEth0.get()) {
    ++metrics.adding_veth_arp_filters_already_exist;

    return Failure(
        "The ARP packet filter from " + veth(pid) +
        " to host " + eth0 + " already exists");
  }

  // Setup filters for ICMP and ARP packets. We mirror ICMP and ARP
  // packets from host eth0 to veths of all the containers. We also
  // setup flow classifiers for host eth0 egress.
  set<string> targets;
  foreachvalue (Info* info, infos) {
    if (info->pid.isSome()) {
      targets.insert(veth(info->pid.get()));
    }
  }

  if (targets.size() == 1) {
    // We just create the first container in which case we should
    // create filters for ICMP and ARP packets.

    // Create a new ICMP filter on host eth0 ingress for mirroring
    // packets from host eth0 to veth.
    Try<bool> icmpEth0ToVeth = filter::icmp::create(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(hostIPNetwork.address()),
        Priority(ICMP_FILTER_PRIORITY, NORMAL),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      ++metrics.adding_eth0_icmp_filters_errors;

      return Failure(
          "Failed to create an ICMP packet filter from host " + eth0 +
          " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      ++metrics.adding_eth0_icmp_filters_already_exist;

      return Failure(
          "The ICMP packet filter on host " + eth0 + " already exists");
    }

    // Create a new ARP filter on host eth0 ingress for mirroring
    // packets from host eth0 to veth.
    Try<bool> arpEth0ToVeth = filter::basic::create(
        eth0,
        ingress::HANDLE,
        ETH_P_ARP,
        Priority(ARP_FILTER_PRIORITY, NORMAL),
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      ++metrics.adding_eth0_arp_filters_errors;

      return Failure(
          "Failed to create an ARP packet filter from host " + eth0 +
          " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      ++metrics.adding_eth0_arp_filters_already_exist;

      return Failure(
          "The ARP packet filter on host " + eth0 + " already exists");
    }
  } else {
    // This is not the first container in which case we should update
    // filters for ICMP and ARP packets.

    // Update the ICMP filter on host eth0 ingress.
    Try<bool> icmpEth0ToVeth = filter::icmp::update(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(hostIPNetwork.address()),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      ++metrics.updating_eth0_icmp_filters_errors;

      return Failure(
          "Failed to append a ICMP mirror action from host " +
           eth0 + " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      ++metrics.updating_eth0_icmp_filters_already_exist;

      return Failure(
          "The ICMP packet filter on host " + eth0 + " already exists");
    }

    // Update the ARP filter on host eth0 ingress.
    Try<bool> arpEth0ToVeth = filter::basic::update(
        eth0,
        ingress::HANDLE,
        ETH_P_ARP,
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      ++metrics.updating_eth0_arp_filters_errors;

      return Failure(
          "Failed to append an ARP mirror action from host " +
           eth0 + " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      ++metrics.updating_eth0_arp_filters_already_exist;

      return Failure(
          "The ARP packet filter on host " + eth0 + " already exists");
    }
  }

  if (flags.egress_unique_flow_per_container) {
    // Create a new ICMP filter on host eth0 egress for classifying
    // packets into a reserved flow.
    Try<bool> icmpEth0Egress = filter::icmp::create(
        eth0,
        hostTxFqCodelHandle,
        icmp::Classifier(None()),
        Priority(ICMP_FILTER_PRIORITY, NORMAL),
        Handle(hostTxFqCodelHandle, ICMP_FLOWID));

    if (icmpEth0Egress.isError()) {
      ++metrics.adding_eth0_egress_filters_errors;

      return Failure(
          "Failed to create the ICMP flow classifier on host " +
          eth0 + ": " + icmpEth0Egress.error());
    } else if (!icmpEth0Egress.get()) {
      // We try to create the filter every time a container is
      // launched. Ignore if it already exists.
    }

    // Create a new ARP filter on host eth0 egress for classifying
    // packets into a reserved flow.
    Try<bool> arpEth0Egress = filter::basic::create(
        eth0,
        hostTxFqCodelHandle,
        ETH_P_ARP,
        Priority(ARP_FILTER_PRIORITY, NORMAL),
        Handle(hostTxFqCodelHandle, ARP_FLOWID));

    if (arpEth0Egress.isError()) {
      ++metrics.adding_eth0_egress_filters_errors;

      return Failure(
          "Failed to create the ARP flow classifier on host " +
          eth0 + ": " + arpEth0Egress.error());
    } else if (!arpEth0Egress.get()) {
      // We try to create the filter every time a container is
      // launched. Ignore if it already exists.
    }

    // Rest of the host packets go to a reserved flow.
    Try<bool> defaultEth0Egress = filter::basic::create(
        eth0,
        hostTxFqCodelHandle,
        ETH_P_ALL,
        Priority(DEFAULT_FILTER_PRIORITY, NORMAL),
        Handle(hostTxFqCodelHandle, HOST_FLOWID));

    if (defaultEth0Egress.isError()) {
      ++metrics.adding_eth0_egress_filters_errors;

      return Failure(
          "Failed to create the default flow classifier on host " +
          eth0 + ": " + defaultEth0Egress.error());
    } else if (!defaultEth0Egress.get()) {
      // NOTE: Since we don't remove this filter on purpose in
      // _cleanup() (see the comments there), we just continue even
      // if it already exists, so do nothing here.
    }
  }

  // Turn on the veth.
  Try<bool> enable = link::setUp(veth(pid));
  if (enable.isError()) {
    return Failure("Failed to turn on " + veth(pid) + ": " + enable.error());
  } else if (!enable.get()) {
    return Failure("Not expecting " + veth(pid) + " to be missing");
  }

  return Nothing();
}


Future<ContainerLimitation> PortMappingIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (unmanaged.contains(containerId)) {
    LOG(WARNING) << "Ignoring watch for unmanaged container " << containerId;
  } else if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring watch for unknown container " << containerId;
  }

  // Currently, we always return a pending future because limitation
  // is never reached.
  return Future<ContainerLimitation>();
}


void PortMappingIsolatorProcess::_update(
    const ContainerID& containerId,
    const Future<Option<int>>& status)
{
  if (!status.isReady()) {
    ++metrics.updating_container_ip_filters_errors;

    LOG(ERROR) << "Failed to start a process for updating container "
               << containerId << ": "
               << (status.isFailed() ? status.failure() : "discarded");
  } else if (status->isNone()) {
    ++metrics.updating_container_ip_filters_errors;

    LOG(ERROR) << "The process for updating container " << containerId
               << " is not expected to be reaped elsewhere";
  } else if (status->get() != 0) {
    ++metrics.updating_container_ip_filters_errors;

    LOG(ERROR) << "The process for updating container " << containerId << " "
               << WSTRINGIFY(status->get());
  } else {
    LOG(INFO) << "The process for updating container " << containerId
              << " finished successfully";
  }
}


Future<Nothing> PortMappingIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  // It is possible for the network isolator to be asked to update a
  // container that isn't managed by it, for instance, containers
  // recovered from a previous run without a network isolator.
  if (unmanaged.contains(containerId)) {
    return Nothing();
  }

  if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  // TODO(jieyu): For now, we simply ignore the 'ephemeral_ports'
  // specified in 'resources'. However, this behavior needs to be
  // changed once the master can make default allocations for
  // ephemeral ports.
  if (resourceRequests.ephemeral_ports().isSome()) {
    LOG(WARNING) << "Ignoring the specified ephemeral_ports '"
                 << resourceRequests.ephemeral_ports().get()
                 << "' for container" << containerId;
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (info->pid.isNone()) {
    return Failure("The container has not been isolated");
  }
  pid_t pid = info->pid.get();

  IntervalSet<uint16_t> nonEphemeralPorts;

  if (resourceRequests.ports().isSome()) {
    nonEphemeralPorts = rangesToIntervalSet<uint16_t>(
        resourceRequests.ports().get()).get();

    // Sanity check to make sure that the assigned non-ephemeral ports
    // for the container are part of the non-ephemeral ports specified
    // by the slave.
    if (!managedNonEphemeralPorts.contains(nonEphemeralPorts)) {
        return Failure(
            "Some non-ephemeral ports specified in " +
            stringify(nonEphemeralPorts) +
            " are not managed by the agent");
    }
  }

  // No need to proceed if no change to the non-ephemeral ports.
  if (nonEphemeralPorts == info->nonEphemeralPorts) {
    return Nothing();
  }

  LOG(INFO) << "Updating non-ephemeral ports for container "
            << containerId << " from " << info->nonEphemeralPorts
            << " to " << nonEphemeralPorts;

  Result<vector<ip::Classifier>> classifiers =
    ip::classifiers(veth(pid), ingress::HANDLE);

  if (classifiers.isError()) {
    return Failure(
        "Failed to get all the IP filters on " + veth(pid) +
        ": " + classifiers.error());
  } else if (classifiers.isNone()) {
    return Failure("Failed to find " + veth(pid));
  }

  // We first decide what port ranges need to be removed. Any filter
  // whose port range is not within the new non-ephemeral ports should
  // be removed.
  hashset<PortRange> portsToRemove;
  IntervalSet<uint16_t> remaining = info->nonEphemeralPorts;

  foreach (const ip::Classifier& classifier, classifiers.get()) {
    Option<PortRange> sourcePorts = classifier.sourcePorts;
    Option<PortRange> destinationPorts = classifier.destinationPorts;

    // All the IP filters on veth used by us only have source ports.
    if (sourcePorts.isNone() || destinationPorts.isSome()) {
      return Failure("Unexpected IP filter detected on " + veth(pid));
    }

    Interval<uint16_t> ports =
      (Bound<uint16_t>::closed(sourcePorts->begin()),
       Bound<uint16_t>::closed(sourcePorts->end()));

    // Skip the ephemeral ports.
    if (ports == info->ephemeralPorts) {
      continue;
    }

    if (!nonEphemeralPorts.contains(ports)) {
      remaining -= ports;
      portsToRemove.insert(sourcePorts.get());
    }
  }

  // We then decide what port ranges need to be added.
  vector<PortRange> portsToAdd = getPortRanges(nonEphemeralPorts - remaining);

  foreach (const PortRange& range, portsToAdd) {
    if (info->flowId.isSome()) {
      LOG(INFO) << "Adding IP packet filters with ports " << range
                << " with flow ID " << info->flowId.get()
                << " for container " << containerId;
    } else {
      LOG(INFO) << "Adding IP packet filters with ports " << range
                << " for container " << containerId;
    }

    // All IP packets from a container will be assigned a single flow
    // on host eth0.
    Try<Nothing> add = addHostIPFilters(range, info->flowId, veth(pid));
    if (add.isError()) {
      return Failure(
          "Failed to add IP packet filter with ports " +
          stringify(range) + " for container with pid " +
          stringify(pid) + ": " + add.error());
    }
  }

  foreach (const PortRange& range, portsToRemove) {
    LOG(INFO) << "Removing IP packet filters with ports " << range
              << " for container with pid " << pid;

    Try<Nothing> removing = removeHostIPFilters(range, veth(pid));
    if (removing.isError()) {
      return Failure(
          "Failed to remove IP packet filter with ports " +
          stringify(range) + " for container with pid " +
          stringify(pid) + ": " + removing.error());
    }
  }

  // Update the non-ephemeral ports of this container.
  info->nonEphemeralPorts = nonEphemeralPorts;

  // Update the IP filters inside the container.
  PortMappingUpdate update;
  update.flags.eth0_name = eth0;
  update.flags.lo_name = lo;
  update.flags.pid = pid;
  update.flags.ports_to_add = json(portsToAdd);
  update.flags.ports_to_remove = json(portsToRemove);

  vector<string> argv(2);
  argv[0] = "mesos-network-helper";
  argv[1] = PortMappingUpdate::NAME;

  Try<Subprocess> s = subprocess(
      path::join(flags.launcher_dir, "mesos-network-helper"),
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      &update.flags);

  if (s.isError()) {
    return Failure("Failed to launch update subcommand: " + s.error());
  }

  return s->status()
    .onAny(defer(
        PID<PortMappingIsolatorProcess>(this),
        &PortMappingIsolatorProcess::_update,
        containerId,
        lambda::_1))
    .then([]() { return Nothing(); });
}


Future<ResourceStatistics> PortMappingIsolatorProcess::usage(
    const ContainerID& containerId)
{
  ResourceStatistics result;

  // Do nothing for unmanaged container.
  if (unmanaged.contains(containerId)) {
    return result;
  }

  if (!infos.contains(containerId)) {
    VLOG(1) << "Unknown container " << containerId;
    return result;
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (info->pid.isNone()) {
    return result;
  }

  Result<hashmap<string, uint64_t>> stat =
    link::statistics(veth(info->pid.get()));

  if (stat.isError()) {
    return Failure(
        "Failed to retrieve statistics on link " +
        veth(info->pid.get()) + ": " + stat.error());
  } else if (stat.isNone()) {
    return Failure("Failed to find link: " + veth(info->pid.get()));
  }

  // Note: The RX/TX statistics on the two ends of the veth are the
  // exact opposite, because of its 'tunnel' nature. We sample on the
  // host end of the veth pair, which means we have to reverse RX and
  // TX to reflect statistics inside the container.

  // +----------+                    +----------+
  // |          |                    |          |
  // |          |RX<--------------+TX|          |
  // |          |                    |          |
  // |   veth   |                    |   eth0   |
  // |          |                    |          |
  // |          |TX+-------------->RX|          |
  // |          |                    |          |
  // +----------+                    +----------+

  Option<uint64_t> rx_packets = stat->get("tx_packets");
  if (rx_packets.isSome()) {
    result.set_net_rx_packets(rx_packets.get());
  }

  Option<uint64_t> rx_bytes = stat->get("tx_bytes");
  if (rx_bytes.isSome()) {
    result.set_net_rx_bytes(rx_bytes.get());
  }

  Option<uint64_t> rx_errors = stat->get("tx_errors");
  if (rx_errors.isSome()) {
    result.set_net_rx_errors(rx_errors.get());
  }

  Option<uint64_t> rx_dropped = stat->get("tx_dropped");
  if (rx_dropped.isSome()) {
    result.set_net_rx_dropped(rx_dropped.get());
  }

  Option<uint64_t> tx_packets = stat->get("rx_packets");
  if (tx_packets.isSome()) {
    result.set_net_tx_packets(tx_packets.get());
  }

  Option<uint64_t> tx_bytes = stat->get("rx_bytes");
  if (tx_bytes.isSome()) {
    result.set_net_tx_bytes(tx_bytes.get());
  }

  Option<uint64_t> tx_errors = stat->get("rx_errors");
  if (tx_errors.isSome()) {
    result.set_net_tx_errors(tx_errors.get());
  }

  Option<uint64_t> tx_dropped = stat->get("rx_dropped");
  if (tx_dropped.isSome()) {
    result.set_net_tx_dropped(tx_dropped.get());
  }

  // Retrieve the socket information from inside the container.
  PortMappingStatistics statistics;
  statistics.flags.pid = info->pid.get();
  statistics.flags.eth0_name = eth0;
  statistics.flags.enable_socket_statistics_summary =
    flags.network_enable_socket_statistics_summary;
  statistics.flags.enable_socket_statistics_details =
    flags.network_enable_socket_statistics_details;
  statistics.flags.enable_snmp_statistics =
    flags.network_enable_snmp_statistics;

  vector<string> argv(2);
  argv[0] = "mesos-network-helper";
  argv[1] = PortMappingStatistics::NAME;

  // We don't need STDIN; we need STDOUT for the result; we leave
  // STDERR as is to log to slave process.
  Try<Subprocess> s = subprocess(
      path::join(flags.launcher_dir, "mesos-network-helper"),
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::FD(STDERR_FILENO),
      &statistics.flags);

  if (s.isError()) {
    return Failure("Failed to launch the statistics subcommand: " + s.error());
  }

  // TODO(chzhcn): it is possible for the subprocess to block on
  // writing to its end of the pipe and never exit because the pipe
  // has limited buffer size, but we have been careful to send very
  // few bytes so this shouldn't be a problem.
  return s->status()
    .then(defer(
        PID<PortMappingIsolatorProcess>(this),
        &PortMappingIsolatorProcess::_usage,
        result,
        s.get()));
}


Future<ResourceStatistics> PortMappingIsolatorProcess::_usage(
    const ResourceStatistics& result,
    const Subprocess& s)
{
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isNone()) {
    return Failure(
        "The process for getting network statistics is unexpectedly reaped");
  } else if (status.get() != 0) {
    return Failure(
        "The process for getting network statistics has non-zero exit code: " +
        WSTRINGIFY(status.get()));
  }

  return io::read(s.out().get())
    .then(defer(
        PID<PortMappingIsolatorProcess>(this),
        &PortMappingIsolatorProcess::__usage,
        result,
        lambda::_1));
}


Future<ResourceStatistics> PortMappingIsolatorProcess::__usage(
    ResourceStatistics result,
    const Future<string>& out)
{
  CHECK_READY(out);

  // NOTE: It's possible the subprocess has no output.
  if (out->empty()) {
    return result;
  }

  Try<JSON::Object> object = JSON::parse<JSON::Object>(out.get());
  if (object.isError()) {
    return Failure(
        "Failed to parse the output from the process that gets the "
        "network statistics: " + object.error());
  }

  Result<ResourceStatistics> _result =
      protobuf::parse<ResourceStatistics>(object.get());

  if (_result.isError()) {
    return Failure(
        "Failed to parse the output from the process that gets the "
        "network statistics: " + object.error());
  }

  result.MergeFrom(_result.get());

  // NOTE: We unset the 'timestamp' field here because otherwise it
  // will overwrite the timestamp set in the containerizer.
  result.clear_timestamp();

  return result;
}


Future<Nothing> PortMappingIsolatorProcess::cleanup(
      const ContainerID& containerId)
{
  if (unmanaged.contains(containerId)) {
    unmanaged.erase(containerId);
    return Nothing();
  }

  if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring cleanup for unknown container " << containerId;
    return Nothing();
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  // For a normally exited container, we take its info pointer off the
  // hashmap infos before using the helper function to clean it up.
  infos.erase(containerId);

  Try<Nothing> cleanup = _cleanup(info, containerId);
  if (cleanup.isError()) {
    return Failure(cleanup.error());
  }

  return Nothing();
}


// TODO(jieyu): We take an optional container ID here because not all
// the containers we want to cleanup have container IDs available. For
// instance, we cannot get container IDs for those orphan containers
// created by older (pre 0.23.0) versions of this isolator (with no
// associated namespace handle symlinks).
Try<Nothing> PortMappingIsolatorProcess::_cleanup(
    Info* _info,
    const Option<ContainerID>& containerId)
{
  // Set '_info' to be auto-managed so that it will be deleted when
  // this function returns.
  Owned<Info> info(CHECK_NOTNULL(_info));

  // Free the ephemeral ports used by the container. Filters
  // associated with those ports will be removed below if they were
  // set up.
  if (info->ephemeralPorts != Interval<uint16_t>()) {
    ephemeralPortsAllocator->deallocate(info->ephemeralPorts);

    LOG(INFO) << "Freed ephemeral ports " << info->ephemeralPorts
              << " used by container"
              << (containerId.isSome()
                  ? " " + stringify(containerId.get()) : "")
              << (info->pid.isSome()
                  ? " with pid " + stringify(info->pid.get()) : "");
  }

  if (!info->pid.isSome()) {
    LOG(WARNING) << "The container has not been isolated";
    return Nothing();
  }

  pid_t pid = info->pid.get();

  // NOTE: The 'isolate()' function above may fail at any point if the
  // child process with 'pid' is gone (e.g., killed by a user, failed
  // to load shared libraries, etc.). Therefore, this cleanup function
  // needs to deal with cases where filters/veth/mount are not
  // installed or do not exist. Also, we choose to continue on each
  // single failure so that we can clean up filters/veth/mount as much
  // as possible. We concatenate all the error messages and report
  // them at the end.
  vector<string> errors;

  // Remove the IP filters on eth0 and lo for non-ephemeral port
  // ranges and the ephemeral port range.
  foreach (const PortRange& range,
           getPortRanges(info->nonEphemeralPorts + info->ephemeralPorts)) {
    LOG(INFO) << "Removing IP packet filters with ports " << range
              << " for container with pid " << pid;

    // No need to remove filters on veth as they will be automatically
    // removed by the kernel when we remove the link below.
    Try<Nothing> removing = removeHostIPFilters(range, veth(pid), false);
    if (removing.isError()) {
      errors.push_back(
          "Failed to remove IP packet filter with ports " +
          stringify(range) + " for container with pid " +
          stringify(pid) + ": " + removing.error());
    }
  }

  if (info->flowId.isSome()) {
    freeFlowIds.insert(info->flowId.get());

    LOG(INFO) << "Freed flow ID " << info->flowId.get()
              << " used by container with pid " << pid;
  }

  set<string> targets;
  foreachvalue (Info* info, infos) {
    if (info->pid.isSome()) {
      targets.insert(veth(info->pid.get()));
    }
  }

  if (targets.empty()) {
    // This is the last container, remove the ARP and ICMP filters on
    // host eth0, remove the flow classifiers on eth0 egress too.

    // Remove the ICMP filter on host eth0.
    Try<bool> icmpEth0ToVeth = filter::icmp::remove(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(hostIPNetwork.address()));

    if (icmpEth0ToVeth.isError()) {
      ++metrics.removing_eth0_icmp_filters_errors;

      errors.push_back(
          "Failed to remove the ICMP packet filter on host " + eth0 +
          ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      ++metrics.removing_eth0_icmp_filters_do_not_exist;

      LOG(ERROR) << "The ICMP packet filter on host " << eth0
                 << " does not exist";
    }

    // Remove the ARP filter on host eth0.
    Try<bool> arpEth0ToVeth = filter::basic::remove(
        eth0,
        ingress::HANDLE,
        ETH_P_ARP);

    if (arpEth0ToVeth.isError()) {
      ++metrics.removing_eth0_arp_filters_errors;

      errors.push_back(
          "Failed to remove the ARP packet filter on host " + eth0 +
          ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      ++metrics.removing_eth0_arp_filters_do_not_exist;

      LOG(ERROR) << "The ARP packet filter on host " << eth0
                 << " does not exist";
    }

    if (flags.egress_unique_flow_per_container) {
      // Remove the ICMP flow classifier on host eth0.
      Try<bool> icmpEth0Egress = filter::icmp::remove(
          eth0,
          hostTxFqCodelHandle,
          icmp::Classifier(None()));

      if (icmpEth0Egress.isError()) {
        ++metrics.removing_eth0_egress_filters_errors;

        errors.push_back(
            "Failed to remove the ICMP flow classifier on host " + eth0 +
            ": " + icmpEth0Egress.error());
      } else if (!icmpEth0Egress.get()) {
        ++metrics.removing_eth0_egress_filters_do_not_exist;

        LOG(ERROR) << "The ICMP flow classifier on host " << eth0
                   << " does not exist";
      }

      // Remove the ARP flow classifier on host eth0.
      Try<bool> arpEth0Egress = filter::basic::remove(
          eth0,
          hostTxFqCodelHandle,
          ETH_P_ARP);

      if (arpEth0Egress.isError()) {
        ++metrics.removing_eth0_egress_filters_errors;

        errors.push_back(
            "Failed to remove the ARP flow classifier on host " + eth0 +
            ": " + arpEth0Egress.error());
      } else if (!arpEth0Egress.get()) {
        ++metrics.removing_eth0_egress_filters_do_not_exist;

        LOG(ERROR) << "The ARP flow classifier on host " << eth0
                   << " does not exist";
      }

      // Kernel creates a place-holder filter, with handle 0, for each
      // tuple (protocol, priority, kind). Our current implementation
      // doesn't remove them, so all these filters are left. Packets
      // will be dropped because these filters don't set a valid flow
      // ID. We have to work around this situation for egress. The
      // long term solution is removing all these filters after our
      // own filters are all gone, see the upstream commit
      // 1e052be69d045c8d0f82ff1116fd3e5a79661745 from:
      // http://git.kernel.org/cgit/linux/kernel/git/davem/net-next.git.
      //
      // So, here we do NOT remove the default flow classifier on host
      // eth0 on purpose so that after all containers are gone the
      // host traffic still goes into this flow, this guarantees no
      // traffic will be dropped by fq_codel qdisc.
      //
      // Maybe we need to remove the fq_codel qdisc on host eth0.
      // TODO(cwang): Revise this in MESOS-2370, we don't remove
      // ingress qdisc either.
    }
  } else {
    // This is not the last container. Replace the ARP and ICMP
    // filters. The reason we do this is that we don't have an easy
    // way to search and delete an action from the multiple actions on
    // a single filter.
    Try<bool> icmpEth0ToVeth = filter::icmp::update(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(hostIPNetwork.address()),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      ++metrics.updating_eth0_icmp_filters_errors;

      errors.push_back(
          "Failed to update the ICMP mirror action from host " + eth0 +
          " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      ++metrics.updating_eth0_icmp_filters_do_not_exist;

      errors.push_back(
          "The ICMP packet filter on host " + eth0 + " does not exist");
    }

    Try<bool> arpEth0ToVeth = filter::basic::update(
        eth0,
        ingress::HANDLE,
        ETH_P_ARP,
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      ++metrics.updating_eth0_arp_filters_errors;

      errors.push_back(
          "Failed to update the ARP mirror action from host " + eth0 +
          " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      ++metrics.updating_eth0_arp_filters_do_not_exist;

      errors.push_back(
          "The ARP packet filter on host " + eth0 + " does not exist");
    }
  }

  // We manually remove veth to avoid having to wait for the kernel to
  // do it.
  Try<bool> remove = link::remove(veth(pid));
  if (remove.isError()) {
    errors.push_back(
        "Failed to remove the link " + veth(pid) + ": " + remove.error());
  }

  // Remove the symlink for the network namespace handle if a
  // container ID is specified.
  if (containerId.isSome()) {
    const string linker = getSymlinkPath(containerId.get());

    // NOTE: Since we introduced the network namespace handle symlink
    // in 0.23.0, it's likely that the symlink does not exist.
    if (os::exists(linker)) {
      Try<Nothing> rm = os::rm(linker);
      if (rm.isError()) {
        errors.push_back(
            "Failed to remove the network namespace symlink '" +
            linker + "' " + rm.error());
      }
    }
  }

  // Release the bind mount for this container.
  const string target = getNamespaceHandlePath(bindMountRoot, pid);
  Try<Nothing> unmount = fs::unmount(target, MNT_DETACH);
  if (unmount.isError()) {
    errors.push_back(
        "Failed to unmount the network namespace handle '" +
        target + "': " + unmount.error());
  }

  // MNT_DETACH does a lazy unmount, which means unmount will
  // eventually succeed when the mount point becomes idle, but
  // possiblely not soon enough every time for this remove to go
  // through, e.g, someone entered into the container for debugging
  // purpose. In that case remove will fail, which is okay, because we
  // only leaked an empty file, which could also be reused later if
  // the pid (the name of the file) is used again.
  Try<Nothing> rm = os::rm(target);
  if (rm.isError()) {
    LOG(WARNING) << "Failed to remove the network namespace handle '"
                 << target << "' during cleanup: " << rm.error();
  }

  // If any error happens along the way, return error.
  if (!errors.empty()) {
    return Error(strings::join(", ", errors));
  }

  LOG(INFO) << "Successfully performed cleanup for pid " << pid;
  return Nothing();
}


// Helper function to set up IP filters on the host side for a given
// port range.
Try<Nothing> PortMappingIsolatorProcess::addHostIPFilters(
    const PortRange& range,
    const Option<uint16_t>& flowId,
    const string& veth)
{
  // NOTE: The order in which these filters are added is important!
  // For each port range, we need to make sure that we don't try to
  // add filters on host eth0 and host lo until we have successfully
  // added filters on veth. This is because the slave could crash
  // while we are adding filters, we want to make sure we don't leak
  // any filters on host eth0 and host lo.

  // Add an IP packet filter from veth of the container to host eth0
  // to properly redirect IP packets sent from one container to
  // external hosts. This filter has a lower priority compared to the
  // 'vethToHostLo' filter because it does not check the destination
  // IP. Notice that here we also check the source port of a packet.
  // If the source port is not within the port ranges allocated for
  // the container, the packet will get dropped.
  Try<bool> vethToHostEth0 = filter::ip::create(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), None(), range, None()),
      Priority(IP_FILTER_PRIORITY, LOW),
      action::Redirect(eth0));

  if (vethToHostEth0.isError()) {
    ++metrics.adding_veth_ip_filters_errors;

    return Error(
        "Failed to create an IP packet filter from " + veth +
        " to host " + eth0 + ": " + vethToHostEth0.error());
  } else if (!vethToHostEth0.get()) {
    ++metrics.adding_veth_ip_filters_already_exist;

    return Error(
        "The IP packet filter from " + veth +
        " to host " + eth0 + " already exists");
  }

  // Add two IP packet filters (one for public IP and one for loopback
  // IP) from veth of the container to host lo to properly redirect IP
  // packets sent from one container to either the host or another
  // container. Notice that here we also check the source port of a
  // packet. If the source port is not within the port ranges
  // allocated for the container, the packet will get dropped.
  Try<bool> vethToHostLoPublic = filter::ip::create(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), hostIPNetwork.address(), range, None()),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(lo));

  if (vethToHostLoPublic.isError()) {
    ++metrics.adding_veth_ip_filters_errors;

    return Error(
        "Failed to create an IP packet filter (for public IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoPublic.error());
  } else if (!vethToHostLoPublic.get()) {
    ++metrics.adding_veth_ip_filters_already_exist;

    return Error(
        "The IP packet filter (for public IP) from " +
        veth + " to host " + lo + " already exists");
  }

  Try<bool> vethToHostLoLoopback = filter::ip::create(
      veth,
      ingress::HANDLE,
      ip::Classifier(
          None(),
          net::IP::Network::LOOPBACK_V4().address(),
          range,
          None()),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(lo));

  if (vethToHostLoLoopback.isError()) {
    ++metrics.adding_veth_ip_filters_errors;

    return Error(
        "Failed to create an IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoLoopback.error());
  } else if (!vethToHostLoLoopback.get()) {
    ++metrics.adding_veth_ip_filters_already_exist;

    return Error(
        "The IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + " already exists");
  }

  // Add an IP packet filter from host eth0 to veth of the container
  // such that any incoming IP packet will be properly redirected to
  // the corresponding container based on its destination port.
  Try<bool> hostEth0ToVeth = filter::ip::create(
      eth0,
      ingress::HANDLE,
      ip::Classifier(hostMAC, hostIPNetwork.address(), None(), range),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(veth));

  if (hostEth0ToVeth.isError()) {
    ++metrics.adding_eth0_ip_filters_errors;

    return Error(
        "Failed to create an IP packet filter from host " +
        eth0 + " to " + veth + ": " + hostEth0ToVeth.error());
  } else if (!hostEth0ToVeth.get()) {
    ++metrics.adding_eth0_ip_filters_already_exist;

    return Error(
        "The IP packet filter from host " + eth0 + " to " +
        veth + " already exists");
  }

  // Add an IP packet filter from host lo to veth of the container
  // such that any internally generated IP packet will be properly
  // redirected to the corresponding container based on its
  // destination port.
  Try<bool> hostLoToVeth = filter::ip::create(
      lo,
      ingress::HANDLE,
      ip::Classifier(None(), None(), None(), range),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(veth));

  if (hostLoToVeth.isError()) {
    ++metrics.adding_lo_ip_filters_errors;

    return Error(
        "Failed to create an IP packet filter from host " +
        lo + " to " + veth + ": " + hostLoToVeth.error());
  } else if (!hostLoToVeth.get()) {
    ++metrics.adding_lo_ip_filters_already_exist;

    return Error(
        "The IP packet filter from host " + lo + " to " +
        veth + " already exists");
  }

  if (flowId.isSome()) {
    // Add IP packet filters to classify traffic sending to eth0
    // in the same way so that traffic of each container will be
    // classified to different flows defined by fq_codel.
    Try<bool> hostEth0Egress = filter::ip::create(
        eth0,
        hostTxFqCodelHandle,
        ip::Classifier(None(), None(), range, None()),
        Priority(IP_FILTER_PRIORITY, LOW),
        Handle(hostTxFqCodelHandle, flowId.get()));

    if (hostEth0Egress.isError()) {
      ++metrics.adding_eth0_egress_filters_errors;

      return Error(
          "Failed to create a flow classifier for " + veth +
          " on host " + eth0 + ": " + hostEth0Egress.error());
    } else if (!hostEth0Egress.get()) {
      ++metrics.adding_eth0_egress_filters_already_exist;

      return Error(
          "The flow classifier for veth " + veth +
          " on host " + eth0 + " already exists");
    }
  }

  return Nothing();
}


// Helper function to remove IP filters from the host side for a given
// port range. The boolean flag 'removeFiltersOnVeth' indicates if we
// need to remove filters on veth.
Try<Nothing> PortMappingIsolatorProcess::removeHostIPFilters(
    const PortRange& range,
    const string& veth,
    bool removeFiltersOnVeth)
{
  // NOTE: Similar to above. The order in which these filters are
  // removed is important. We need to remove filters on host eth0 and
  // host lo first before we remove filters on veth.

  // Remove the IP packet filter from host eth0 to veth of the container.
  Try<bool> hostEth0ToVeth = filter::ip::remove(
      eth0,
      ingress::HANDLE,
      ip::Classifier(hostMAC, hostIPNetwork.address(), None(), range));

  if (hostEth0ToVeth.isError()) {
    ++metrics.removing_eth0_ip_filters_errors;

    return Error(
        "Failed to remove the IP packet filter from host " +
        eth0 + " to " + veth + ": " + hostEth0ToVeth.error());
  } else if (!hostEth0ToVeth.get()) {
    ++metrics.removing_eth0_ip_filters_do_not_exist;

    LOG(ERROR) << "The IP packet filter from host " << eth0
               << " to " << veth << " does not exist";
  }

  // Remove the IP packet filter from host lo to veth of the container.
  Try<bool> hostLoToVeth = filter::ip::remove(
      lo,
      ingress::HANDLE,
      ip::Classifier(None(), None(), None(), range));

  if (hostLoToVeth.isError()) {
    ++metrics.removing_lo_ip_filters_errors;

    return Error(
        "Failed to remove the IP packet filter from host " +
        lo + " to " + veth + ": " + hostLoToVeth.error());
  } else if (!hostLoToVeth.get()) {
    ++metrics.removing_lo_ip_filters_do_not_exist;

    LOG(ERROR) << "The IP packet filter from host " << lo
               << " to " << veth << " does not exist";
  }

  if (flags.egress_unique_flow_per_container) {
    // Remove the egress flow classifier on host eth0.
    Try<bool> hostEth0Egress = filter::ip::remove(
        eth0,
        hostTxFqCodelHandle,
        ip::Classifier(None(), None(), range, None()));

    if (hostEth0Egress.isError()) {
      ++metrics.removing_eth0_egress_filters_errors;

      return Error(
          "Failed to remove the flow classifier from host " +
          eth0 + " for " + veth + ": " + hostEth0Egress.error());
    } else if (!hostEth0Egress.get()) {
      ++metrics.removing_eth0_egress_filters_do_not_exist;

      LOG(ERROR) << "The flow classifier from host " << eth0
                 << " for " << range << " does not exist";
    }
  }

  // Now, we try to remove filters on veth. No need to proceed if the
  // user does not ask us to do so.
  if (!removeFiltersOnVeth) {
    return Nothing();
  }

  // Remove the IP packet filter from veth of the container to
  // host lo for the public IP.
  Try<bool> vethToHostLoPublic = filter::ip::remove(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), hostIPNetwork.address(), range, None()));

  if (vethToHostLoPublic.isError()) {
    ++metrics.removing_lo_ip_filters_errors;

    return Error(
        "Failed to remove the IP packet filter (for public IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoPublic.error());
  } else if (!vethToHostLoPublic.get()) {
    ++metrics.removing_lo_ip_filters_do_not_exist;

    LOG(ERROR) << "The IP packet filter (for public IP) from "
               << veth << " to host " << lo << " does not exist";
  }

  // Remove the IP packet filter from veth of the container to
  // host lo for the loopback IP.
  Try<bool> vethToHostLoLoopback = filter::ip::remove(
      veth,
      ingress::HANDLE,
      ip::Classifier(
          None(),
          net::IP::Network::LOOPBACK_V4().address(),
          range,
          None()));

  if (vethToHostLoLoopback.isError()) {
    ++metrics.removing_veth_ip_filters_errors;

    return Error(
        "Failed to remove the IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoLoopback.error());
  } else if (!vethToHostLoLoopback.get()) {
    ++metrics.removing_veth_ip_filters_do_not_exist;

    LOG(ERROR) << "The IP packet filter (for loopback IP) from "
               << veth << " to host " << lo << " does not exist";
  }

  // Remove the IP packet filter from veth of the container to
  // host eth0.
  Try<bool> vethToHostEth0 = filter::ip::remove(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), None(), range, None()));

  if (vethToHostEth0.isError()) {
    ++metrics.removing_veth_ip_filters_errors;

    return Error(
        "Failed to remove the IP packet filter from " + veth +
        " to host " + eth0 + ": " + vethToHostEth0.error());
  } else if (!vethToHostEth0.get()) {
    ++metrics.removing_veth_ip_filters_do_not_exist;

    LOG(ERROR) << "The IP packet filter from " << veth
               << " to host " << eth0 << " does not exist";
  }

  return Nothing();
}


// This function returns the scripts that need to be run in child
// context before child execs to complete network isolation.
// TODO(jieyu): Use the Subcommand abstraction to remove most of the
// logic here. Completely remove this function once we can assume a
// newer kernel where 'setns' works for mount namespaces.
string PortMappingIsolatorProcess::scripts(Info* info)
{
  ostringstream script;

  script << "#!/bin/sh\n";
  script << "set -xe\n";

  // Mark the mount point PORT_MAPPING_BIND_MOUNT_ROOT() as slave
  // mount so that changes in the container will not be propagated to
  // the host.
  script << "mount --make-rslave " << bindMountRoot << "\n";

  // Disable IPv6 when IPv6 module is loaded as IPv6 packets won't be
  // forwarded anyway.
  script << "test -f /proc/sys/net/ipv6/conf/all/disable_ipv6 &&"
         << " echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6\n";

  // Configure lo and eth0.
  script << "ip link set " << lo << " address " << hostMAC
         << " mtu " << hostEth0MTU << " up\n";

  // NOTE: This is mostly a kernel issue: in veth_xmit() the kernel
  // tags the packet's checksum as UNNECESSARY if we do not disable it
  // here, this causes a corrupt packet to be delivered into the stack
  // when we receive a packet with a bad checksum. Disabling rx
  // checksum offloading ensures the TCP layer will checksum and drop
  // it.
  script << "ethtool -K " << eth0 << " rx off\n";
  script << "ip link set " << eth0 << " address " << hostMAC
         << " mtu " << hostEth0MTU << " up\n";
  script << "ip addr add " << hostIPNetwork << " dev " << eth0 << "\n";

  // Set up the default gateway to match that of eth0.
  script << "ip route add default via " << hostDefaultGateway << "\n";

  // Restrict the ephemeral ports that can be used by the container.
  script << "echo " << info->ephemeralPorts.lower() << " "
         << (info->ephemeralPorts.upper() - 1)
         << " > /proc/sys/net/ipv4/ip_local_port_range\n";

  // Allow eth0 and lo in the container to accept local packets. We
  // need this because we will set up filters to redirect packets from
  // lo to eth0 in the container.
  script << "echo 1 > /proc/sys/net/ipv4/conf/" << eth0 << "/accept_local\n";
  script << "echo 1 > /proc/sys/net/ipv4/conf/" << lo << "/accept_local\n";

  // Enable route_localnet on lo because by default 127.0.0.1 traffic
  // is dropped. This feature exists on 3.6 kernel or newer.
  if (os::exists(path::join("/proc/sys/net/ipv4/conf", lo, "route_localnet"))) {
    script << "echo 1 > /proc/sys/net/ipv4/conf/" << lo << "/route_localnet\n";
  }

  // Configure container network to match host network configurations.
  foreachpair (const string& proc,
               const string& value,
               hostNetworkConfigurations) {
    script << "if [ -f \"" << proc << "\" ]; then\n";
    script << " echo '" << value << "' > " << proc << "\n";
    script << "fi\n";
  }

  // Set up filters on lo and eth0.
  script << "tc qdisc add dev " << lo << " ingress\n";
  script << "tc qdisc add dev " << eth0 << " ingress\n";

  // Allow talking between containers and from container to host.
  // TODO(chzhcn): Consider merging the following two filters.
  script << "tc filter add dev " << lo << " parent " << ingress::HANDLE
         << " protocol ip"
         << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip dst " << hostIPNetwork.address()
         << " action mirred egress redirect dev " << eth0 << "\n";

  script << "tc filter add dev " << lo << " parent " << ingress::HANDLE
         << " protocol ip"
         << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip dst "
         << net::IP::Network::LOOPBACK_V4().address()
         << " action mirred egress redirect dev " << eth0 << "\n";

  foreach (const PortRange& range,
           getPortRanges(info->nonEphemeralPorts + info->ephemeralPorts)) {
    // Local traffic inside a container will not be redirected to eth0.
    script << "tc filter add dev " << lo << " parent " << ingress::HANDLE
           << " protocol ip"
           << " prio " << Priority(IP_FILTER_PRIORITY, HIGH).get() << " u32"
           << " flowid ffff:0"
           << " match ip dport " << range.begin() << " "
           << hex << range.mask() << dec << "\n";

    // Traffic going to host loopback IP and ports assigned to this
    // container will be redirected to lo.
    script << "tc filter add dev " << eth0 << " parent " << ingress::HANDLE
           << " protocol ip"
           << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
           << " flowid ffff:0"
           << " match ip dst "
           << net::IP::Network::LOOPBACK_V4().address()
           << " match ip dport " << range.begin() << " "
           << hex << range.mask() << dec
           << " action mirred egress redirect dev " << lo << "\n";
  }

  // Do not forward the ICMP packet if the destination IP is self.
  script << "tc filter add dev " << lo << " parent " << ingress::HANDLE
         << " protocol ip"
         << " prio " << Priority(ICMP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip protocol 1 0xff"
         << " match ip dst " << hostIPNetwork.address() << "\n";

  script << "tc filter add dev " << lo << " parent " << ingress::HANDLE
         << " protocol ip"
         << " prio " << Priority(ICMP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip protocol 1 0xff"
         << " match ip dst "
         << net::IP::Network::LOOPBACK_V4().address() << "\n";

  // Display the filters created on eth0 and lo.
  script << "tc filter show dev " << eth0
         << " parent " << ingress::HANDLE << "\n";
  script << "tc filter show dev " << lo
         << " parent " << ingress::HANDLE << "\n";

  // If throughput limit for container egress traffic exists, use HTB
  // qdisc to achieve traffic shaping.
  // TBF has some known issues with GSO packets.
  // https://git.kernel.org/cgit/linux/kernel/git/davem/net.git/:
  // e43ac79a4bc6ca90de4ba10983b4ca39cd215b4b
  // Additionally, HTB has a simpler interface for just capping the
  // throughput. TBF requires other parameters such as 'burst' that
  // HTB already has default values for.
  if (egressRateLimitPerContainer.isSome()) {
    script << "tc qdisc add dev " << eth0 << " root handle "
           << CONTAINER_TX_HTB_HANDLE << " htb default 1\n";
    script << "tc class add dev " << eth0 << " parent "
           << CONTAINER_TX_HTB_HANDLE << " classid "
           << CONTAINER_TX_HTB_CLASS_ID << " htb rate "
           << egressRateLimitPerContainer->bytes() * 8 << "bit\n";

    // Packets are buffered at the leaf qdisc if we send them faster
    // than the HTB rate limit and may be dropped when the queue is
    // full, so we change the default leaf qdisc from pfifo_fast to
    // fq_codel, which has a larger buffer and better control on
    // buffer bloat.
    // TODO(cwang): Verity that fq_codel qdisc is available.
    script << "tc qdisc add dev " << eth0
           << " parent " << CONTAINER_TX_HTB_CLASS_ID << " fq_codel\n";

    // Display the htb qdisc and class created on eth0.
    script << "tc qdisc show dev " << eth0 << "\n";
    script << "tc class show dev " << eth0 << "\n";
  }

  return script.str();
}


uint16_t PortMappingIsolatorProcess::getNextFlowId()
{
  // NOTE: It is very unlikely that we exhaust all the flow IDs.
  CHECK(freeFlowIds.begin() != freeFlowIds.end());

  uint16_t flowId = *freeFlowIds.begin();

  freeFlowIds.erase(freeFlowIds.begin());

  return flowId;
}


////////////////////////////////////////////////////
// Implementation for the ephemeral ports allocator.
////////////////////////////////////////////////////


uint32_t EphemeralPortsAllocator::nextMultipleOf(uint32_t x, uint32_t m)
{
  uint32_t div = x / m;
  uint32_t mod = x % m;

  return (div + (mod == 0 ? 0 : 1)) * m;
}


Try<Interval<uint16_t>> EphemeralPortsAllocator::allocate()
{
  if (portsPerContainer_ == 0) {
    return Error("Number of ephemeral ports per container is zero");
  }

  Option<Interval<uint16_t>> allocated;

  foreach (const Interval<uint16_t>& interval, free) {
    uint16_t upper = interval.upper();
    uint16_t lower = interval.lower();
    uint16_t size = upper - lower;

    if (size < portsPerContainer_) {
      continue;
    }

    // If 'lower' is not aligned, calculate the new aligned 'lower'.
    if (lower % portsPerContainer_ != 0) {
      lower = nextMultipleOf(lower, portsPerContainer_);
      if (lower + portsPerContainer_ > upper) {
        continue;
      }
    }

    allocated = (Bound<uint16_t>::closed(lower),
                 Bound<uint16_t>::open(lower + portsPerContainer_));
    break;
  }

  if (allocated.isNone()) {
    return Error("Failed to allocate ephemeral ports");
  }

  allocate(allocated.get());

  return allocated.get();
}


void EphemeralPortsAllocator::allocate(const Interval<uint16_t>& ports)
{
  CHECK(free.contains(ports));
  CHECK(!used.contains(ports));
  free -= ports;
  used += ports;
}


void EphemeralPortsAllocator::deallocate(const Interval<uint16_t>& ports)
{
  CHECK(!free.contains(ports));
  CHECK(used.contains(ports));
  free += ports;
  used -= ports;
}


// This function is exposed for unit testing.
vector<PortRange> getPortRanges(const IntervalSet<uint16_t>& ports)
{
  vector<PortRange> ranges;

  foreach (const Interval<uint16_t>& interval, ports) {
    uint16_t lower = interval.lower(); // Inclusive lower.
    uint16_t upper = interval.upper(); // Exclusive upper.

    // Construct a set of valid port ranges (i.e., that can be used by
    // a filter) from 'interval'. We keep incrementing 'lower' as we
    // find valid port ranges until we reach 'upper'.
    while (lower < upper) {
      // Determine the size of the port range starting from 'lower'.
      // The size has to satisfy the following conditions: 1) size =
      // 2^n (n=0,1,2,...); 2) lower % size == 0.
      size_t size;
      for (size = roundDownToPowerOfTwo(lower) ; size > 1; size = size / 2) {
        if (lower % size == 0 && lower + size <= upper) {
          break;
        }
      }

      // Construct the port range given the size.
      Try<PortRange> range = PortRange::fromBeginEnd(lower, lower + size - 1);

      CHECK_SOME(range) << "Invalid port range: " << "[" << lower << ","
                        << (lower + size - 1) << "]";

      ranges.push_back(range.get());

      lower += size;
    }
  }

  return ranges;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
