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

#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/setns.hpp>

#include "linux/fs.hpp"

#include "linux/routing/route.hpp"
#include "linux/routing/utils.hpp"

#include "linux/routing/filter/arp.hpp"
#include "linux/routing/filter/icmp.hpp"
#include "linux/routing/filter/ip.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/ingress.hpp"

#include "mesos/resources.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/isolators/network/port_mapping.hpp"

using namespace mesos::internal;

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;

using std::cerr;
using std::dec;
using std::endl;
using std::hex;
using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using filter::ip::PortRange;

namespace mesos {
namespace internal {
namespace slave {

const std::string VETH_PREFIX = "mesos";


// The root directory where we bind mount all the namespace handles.
// We choose the directory '/var/run/netns' so that we can use
// iproute2 suite (e.g., ip netns show/exec) to inspect or enter the
// network namespace. This is very useful for debugging purposes.
const string BIND_MOUNT_ROOT = "/var/run/netns";


// The minimum number of ephemeral ports a container should have.
static const uint16_t MIN_EPHEMERAL_PORTS_SIZE =
    DEFAULT_EPHEMERAL_PORTS_PER_CONTAINER;


// The primary priority used by each type of filter.
static const uint8_t ARP_FILTER_PRIORITY = 1;
static const uint8_t ICMP_FILTER_PRIORITY = 2;
static const uint8_t IP_FILTER_PRIORITY = 3;


// The secondary priorities used by filters.
static const uint8_t HIGH = 1;
static const uint8_t NORMAL = 2;
static const uint8_t LOW = 3;


// The loopback IP reserved by IPv4 standard.
// TODO(jieyu): Support IP filters for the entire subnet.
static net::IP LOOPBACK_IP = net::IP::fromDotDecimal("127.0.0.1/8").get();


// The well known ports. Used for sanity check.
static const Interval<uint16_t> WELL_KNOWN_PORTS =
  (Bound<uint16_t>::closed(0), Bound<uint16_t>::open(1024));


/////////////////////////////////////////////////
// Helper functions for the isolator.
/////////////////////////////////////////////////

static Future<Nothing> _nothing() { return Nothing(); }


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
  return VETH_PREFIX + stringify(pid);
}


// Extracts the pid from the given veth name.
static Option<pid_t> getPid(string veth)
{
  if (strings::startsWith(veth, VETH_PREFIX)) {
    Try<pid_t> pid = numify<pid_t>(
        strings::remove(veth, VETH_PREFIX, strings::PREFIX));

    if (pid.isSome()) {
      return pid.get();
    }
  }

  return None();
}


// Converts from value ranges to interval set.
static IntervalSet<uint16_t> getIntervalSet(const Value::Ranges& ranges)
{
  IntervalSet<uint16_t> set;

  for (int i = 0; i < ranges.range_size(); i++) {
    set += (Bound<uint16_t>::closed(ranges.range(i).begin()),
            Bound<uint16_t>::closed(ranges.range(i).end()));
  }

  return set;
}


// Helper function to set up IP filters on the host side for a given
// port range.
static Try<Nothing> addHostIPFilters(
    const PortRange& range,
    const string& eth0,
    const string& lo,
    const string& veth,
    const net::MAC& hostMAC,
    const net::IP& hostIP)
{
  // Add an IP packet filter from host eth0 to veth of the container
  // such that any incoming IP packet will be properly redirected to
  // the corresponding container based on its destination port.
  Try<bool> hostEth0ToVeth = filter::ip::create(
      eth0,
      ingress::HANDLE,
      ip::Classifier(hostMAC, net::IP(hostIP.address()), None(), range),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(veth));

  if (hostEth0ToVeth.isError()) {
    return Error(
        "Failed to create an IP packet filter from host " +
        eth0 + " to " + veth + ": " + hostEth0ToVeth.error());
  } else if (!hostEth0ToVeth.get()) {
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
    return Error(
        "Failed to create an IP packet filter from host " +
        lo + " to " + veth + ": " + hostLoToVeth.error());
  } else if (!hostLoToVeth.get()) {
    return Error(
        "The IP packet filter from host " + lo + " to " +
        veth + " already exists");
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
      ip::Classifier(None(), net::IP(hostIP.address()), range, None()),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(lo));

  if (vethToHostLoPublic.isError()) {
    return Error(
        "Failed to create an IP packet filter (for public IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoPublic.error());
  } else if (!vethToHostLoPublic.get()) {
    return Error(
        "The IP packet filter (for public IP) from " +
        veth + " to host " + lo + " already exists");
  }

  Try<bool> vethToHostLoLoopback = filter::ip::create(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), net::IP(LOOPBACK_IP.address()), range, None()),
      Priority(IP_FILTER_PRIORITY, NORMAL),
      action::Redirect(lo));

  if (vethToHostLoLoopback.isError()) {
    return Error(
        "Failed to create an IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoLoopback.error());
  } else if (!vethToHostLoLoopback.get()) {
    return Error(
        "The IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + " already exists");
  }

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
    return Error(
        "Failed to create an IP packet filter from " + veth +
        " to host " + eth0 + ": " + vethToHostEth0.error());
  } else if (!vethToHostEth0.get()) {
    return Error(
        "The IP packet filter from " + veth +
        " to host " + eth0 + " already exists");
  }

  return Nothing();
}


// Helper function to remove IP filters from the host side for a given
// port range.
static Try<Nothing> removeHostIPFilters(
    const PortRange& range,
    const string& eth0,
    const string& lo,
    const string& veth,
    const net::MAC& hostMAC,
    const net::IP& hostIP)
{
  // Remove the IP packet filter from host eth0 to veth of the container
  Try<bool> hostEth0ToVeth = filter::ip::remove(
      eth0,
      ingress::HANDLE,
      ip::Classifier(hostMAC, net::IP(hostIP.address()), None(), range));

  if (hostEth0ToVeth.isError()) {
    return Error(
        "Failed to remove the IP packet filter from host " +
        eth0 + " to " + veth + ": " + hostEth0ToVeth.error());
  } else if (!hostEth0ToVeth.get()) {
    LOG(ERROR) << "The IP packet filter from host " << eth0
               << " to " << veth << " does not exist";
  }

  // Remove the IP packet filter from host lo to veth of the container
  Try<bool> hostLoToVeth = filter::ip::remove(
      lo,
      ingress::HANDLE,
      ip::Classifier(None(), None(), None(), range));

  if (hostLoToVeth.isError()) {
    return Error(
        "Failed to remove the IP packet filter from host " +
        lo + " to " + veth + ": " + hostLoToVeth.error());
  } else if (!hostLoToVeth.get()) {
    LOG(ERROR) << "The IP packet filter from host " << lo
               << " to " << veth << " does not exist";
  }

  // Remove the IP packet filter from veth of the container to
  // host lo for the public IP.
  Try<bool> vethToHostLoPublic = filter::ip::remove(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), net::IP(hostIP.address()), range, None()));

  if (vethToHostLoPublic.isError()) {
    return Error(
        "Failed to remove the IP packet filter (for public IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoPublic.error());
  } else if (!vethToHostLoPublic.get()) {
    LOG(ERROR) << "The IP packet filter (for public IP) from "
               << veth << " to host " << lo << " does not exist";
  }

  // Remove the IP packet filter from veth of the container to
  // host lo for the loopback IP.
  Try<bool> vethToHostLoLoopback = filter::ip::remove(
      veth,
      ingress::HANDLE,
      ip::Classifier(None(), net::IP(LOOPBACK_IP.address()), range, None()));

  if (vethToHostLoLoopback.isError()) {
    return Error(
        "Failed to remove the IP packet filter (for loopback IP) from " +
        veth + " to host " + lo + ": " + vethToHostLoLoopback.error());
  } else if (!vethToHostLoLoopback.get()) {
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
    return Error(
        "Failed to remove the IP packet filter from " + veth +
        " to host " + eth0 + ": " + vethToHostEth0.error());
  } else if (!vethToHostEth0.get()) {
    LOG(ERROR) << "The IP packet filter from " << veth
               << " to host " << eth0 << " does not exist";
  }

  return Nothing();
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
      ip::Classifier(None(), net::IP(LOOPBACK_IP.address()), None(), range),
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
      ip::Classifier(None(), net::IP(LOOPBACK_IP.address()), None(), range));

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

/////////////////////////////////////////////////
// Implementation for PortMappingUpdate.
/////////////////////////////////////////////////

const std::string PortMappingUpdate::NAME = "update";


PortMappingUpdate::Flags::Flags()
{
  add(&help,
      "help",
      "Prints this help message",
      false);

  add(&eth0_name,
      "eth0_name",
      "The name of the public network interface (e.g., eth0)");

  add(&lo_name,
      "lo_name",
      "The name of the loopback network interface (e.g., lo)");

  add(&pid,
      "pid",
      "The pid of the process whose namespaces we will enter");

  add(&ports_to_add,
      "ports_to_add",
      "A collection of port ranges (formatted as a JSON object)\n"
      "for which to add IP filters. E.g.,\n"
      "--ports_to_add={\"range\":[{\"begin\":4,\"end\":8}]}");

  add(&ports_to_remove,
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
  return JSON::Protobuf(values);
}


static Try<vector<PortRange> > parse(const JSON::Object& object)
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

  Option<vector<PortRange> > portsToAdd;
  Option<vector<PortRange> > portsToRemove;

  if (flags.ports_to_add.isSome()) {
    Try<vector<PortRange> > parsing = parse(flags.ports_to_add.get());
    if (parsing.isError()) {
      cerr << "Parsing 'ports_to_add' failed: " << parsing.error() << endl;
      return 1;
    }
    portsToAdd = parsing.get();
  }

  if (flags.ports_to_remove.isSome()) {
    Try<vector<PortRange> > parsing = parse(flags.ports_to_remove.get());
    if (parsing.isError()) {
      cerr << "Parsing 'ports_to_remove' failed: " << parsing.error() << endl;
      return 1;
    }
    portsToRemove = parsing.get();
  }

  // Enter the network namespace.
  Try<Nothing> setns = os::setns(flags.pid.get(), "net");
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
// Implementation for the isolator.
/////////////////////////////////////////////////

Try<Isolator*> PortMappingIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("Using network isolator requires root permissions");
  }

  // Verify that the network namespace is available by checking the
  // existence of the network namespace handle of the current process.
  if (os::namespaces().count("net") == 0) {
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
  Try<int> checkCommandTc = os::shell(NULL, "tc filter show");
  if (checkCommandTc.isError()) {
    return Error("Check command 'tc' failed: " + checkCommandTc.error());
  } else if (checkCommandTc.get() != 0) {
    return Error(
        "Check command 'tc' failed: non-zero exit code: " +
        stringify(checkCommandTc.get()));
  }

  Try<int> checkCommandIp = os::shell(NULL, "ip link show");
  if (checkCommandIp.isError()) {
    return Error("Check command 'ip' failed: " + checkCommandIp.error());
  } else if (checkCommandIp.get() != 0) {
    return Error(
        "Check command 'ip' failed: non-zero exit code: " +
        stringify(checkCommandIp.get()));
  }

  // Get 'ports' resource from 'resources' flag. These ports will be
  // treated as non-ephemeral ports.
  IntervalSet<uint16_t> nonEphemeralPorts;

  Try<Resources> resources = Resources::parse(
      flags.resources.get(""),
      flags.default_role);

  if (resources.isError()) {
    return Error("Failed to parse --resources: " + resources.error());
  }

  if (resources.get().ports().isSome()) {
    nonEphemeralPorts = getIntervalSet(resources.get().ports().get());
  }

  // Get 'ports' resource from 'private_resources' flag. These ports
  // will be allocated to each container as ephemeral ports.
  IntervalSet<uint16_t> ephemeralPorts;

  resources = Resources::parse(
      flags.private_resources.get(""),
      flags.default_role);

  if (resources.isError()) {
    return Error("Failed to parse --private_resources: " + resources.error());
  }

  if (resources.get().ports().isSome()) {
    ephemeralPorts = getIntervalSet(resources.get().ports().get());
  }

  // Each container requires at least one ephemeral port for slave
  // executor communication. If no 'ports' resource is found, we will
  // return error.
  if (ephemeralPorts.empty()) {
    return Error("Private resources do not contain ports");
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
  if (ephemeralPorts.intersects(WELL_KNOWN_PORTS)) {
    return Error(
        "The specified ephemeral ports " + stringify(ephemeralPorts) +
        " intersect with well known ports " + stringify(WELL_KNOWN_PORTS));
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
        "Network Isolator failed to find a public interface: " + eth0.error());
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
        "Network Isolator failed to find a loopback interface: " + lo.error());
  }

  LOG(INFO) << "Using " << lo.get() << " as the loopback interface";

  // Get the host IP, MAC and default gateway.
  Result<net::IP> hostIP = net::ip(eth0.get());
  if (!hostIP.isSome()) {
    return Error(
        "Failed to get the public IP of " + eth0.get() + ": " +
        (hostIP.isError() ? hostIP.error() : "does not have an IPv4 address"));
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
  Try<bool> createHostEth0Qdisc = ingress::create(eth0.get());
  if (createHostEth0Qdisc.isError()) {
    return Error(
        "Failed to create the ingress qdisc on " + eth0.get() +
        ": " + createHostEth0Qdisc.error());
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

  // Self bind mount BIND_MOUNT_ROOT. Since we use a new mount
  // namespace for each container, for this mount point, we set
  // '--make-rshared' on the host and set '--make-rslave' inside each
  // container. This is important because when we unmount the network
  // namespace handles on the host, those handles will be unmounted in
  // the containers as well, but NOT vice versa.

  // We first create the bind mount directory if it does not exist.
  Try<Nothing> mkdir = os::mkdir(BIND_MOUNT_ROOT);
  if(mkdir.isError()) {
    return Error(
        "Failed to create the bind mount root directory at " +
        BIND_MOUNT_ROOT + ": " + mkdir.error());
  }

  // Now, check '/proc/mounts' to see if BIND_MOUNT_ROOT has already
  // been self mounted.
  Try<fs::MountTable> mountTable = fs::MountTable::read("/proc/mounts");
  if (mountTable.isError()) {
    return Error(
        "Failed to the read the mount table at '/proc/mounts': " +
        mountTable.error());
  }

  Option<fs::MountTable::Entry> bindMountRoot;
  foreach (const fs::MountTable::Entry& entry, mountTable.get().entries) {
    if (entry.dir == BIND_MOUNT_ROOT) {
      bindMountRoot = entry;
    }
  }

  // Self bind mount BIND_MOUNT_ROOT.
  if (bindMountRoot.isNone()) {
    // NOTE: Instead of using fs::mount to perform the bind mount, we
    // use the shell command here because the syscall 'mount' does not
    // update the mount table (i.e., /etc/mtab), which could cause
    // issues for the shell command 'mount --make-rslave' inside the
    // container. It's OK to use the blocking os::shell here because
    // 'create' will only be invoked during initialization.
    Try<int> mount = os::shell(
        NULL,
        "mount --bind %s %s",
        BIND_MOUNT_ROOT.c_str(),
        BIND_MOUNT_ROOT.c_str());

    if (mount.isError()) {
      return Error(
          "Failed to self bind mount '" + BIND_MOUNT_ROOT +
          "': " + mount.error());
    } else if (mount.get() != 0) {
      return Error(
          "Failed to self bind mount '" + BIND_MOUNT_ROOT +
          "': non-zero exit code: " + stringify(mount.get()));
    }
  }

  // Mark the mount point BIND_MOUNT_ROOT as recursively shared.
  Try<int> mountShared = os::shell(
      NULL,
      "mount --make-rshared %s",
      BIND_MOUNT_ROOT.c_str());

  if (mountShared.isError()) {
    return Error(
        "Failed to mark '" + BIND_MOUNT_ROOT + "' as recursively shared: " +
        mountShared.error());
  } else if (mountShared.get() != 0) {
    return Error(
        "Failed to mark '" + BIND_MOUNT_ROOT + "' as recursively shared: " +
        "non-zero exit code: " + stringify(mountShared.get()));
  }

  return new Isolator(Owned<IsolatorProcess>(
      new PortMappingIsolatorProcess(
          flags,
          eth0.get(),
          lo.get(),
          hostMAC.get(),
          hostIP.get(),
          hostEth0MTU.get(),
          hostDefaultGateway.get(),
          nonEphemeralPorts,
          ephemeralPortsAllocator)));
}


Future<Nothing> PortMappingIsolatorProcess::recover(
    const list<state::RunState>& states)
{
  // Extract pids from virtual device names.
  Try<set<string> > links = net::links();
  if (links.isError()) {
    return Failure("Failed to get all the links: " + links.error());
  }

  hashset<pid_t> pids;
  foreach (const string& name, links.get()) {
    Option<pid_t> pid = getPid(name);
    // Not all links follow the naming: mesos{pid}, so we simply
    // continue, e.g., eth0.
    if (pid.isNone()) {
      continue;
    } else if (pids.contains(pid.get())) {
      return Failure("Two virtual devices have the same name '" + name + "'");
    }

    pids.insert(pid.get());
  }

  foreach (const state::RunState& state, states) {
    if (!state.id.isSome()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      unmanaged.clear();

      return Failure("ContainerID and pid are required to recover");
    }

    // Containerizer is not supposed to let the isolator recover a run
    // with a forked pid.
    CHECK_SOME(state.forkedPid);

    const ContainerID& containerId = state.id.get();
    pid_t pid = state.forkedPid.get();

    VLOG(1) << "Recovering network isolator for container "
            << containerId << " with pid " << pid;

    if (!pids.contains(pid)) {
      // This is possible because the container was launched by the
      // slave with network isolation disabled, so the pid could not
      // be found in the device names in the system.
      VLOG(1) << "Skipped recovery for container " << containerId
              << "with pid " << pid << " as it was not managed by "
              << "the network isolator";
      unmanaged.insert(containerId);
      continue;
    }

    Result<Info*> recover = _recover(pid);
    if (recover.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      unmanaged.clear();

      return Failure(
          "Failed to recover container " + stringify(containerId) +
          " with pid " + stringify(pid) + ": " + recover.error());
    } else if (recover.isNone()) {
      LOG(WARNING) << "Cannot recover container " << containerId
                   << " with pid " << pid
                   << ". It may have already been destroyed";

      // This may occur if the executor has exited and the isolator
      // has destroyed the container but the slave dies before
      // noticing this.
      continue;
    }

    infos[containerId] = recover.get();

    // Remove the successfully recovered pid.
    pids.erase(pid);
  }

  // If there are orphaned containers left, clean them up.
  foreach (pid_t pid, pids) {
    Result<Info*> recover = _recover(pid);
    if (recover.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      unmanaged.clear();

      return Failure(
          "Failed to recover orphaned container with pid " +
          stringify(pid) + ": " + recover.error());
    } else if (recover.isNone()) {
      // If the control reaches here, a serious problem has occurred
      // because our link (veth) has been unexpectedly deleted.
      LOG(FATAL) << "The veth for orphaned container with pid "
                 << pid << " has been unexpectedly deleted";
    }

    // The recovery should fail if we cannot cleanup an orphan.
    Try<Nothing> cleanup = _cleanup(recover.get());
    if (cleanup.isError()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      unmanaged.clear();

      return Failure(
          "Failed to cleanup orphaned container with pid " +
          stringify(pid) + ": " + cleanup.error());
    }
  }

  LOG(INFO) << "Network isolator recovery complete";

  return Nothing();
}


Result<PortMappingIsolatorProcess::Info*>
PortMappingIsolatorProcess::_recover(pid_t pid)
{
  // Get all the IP filters on veth.
  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(veth(pid), ingress::HANDLE);

  if (classifiers.isError()) {
    return Error(
        "Failed to get all the IP filters on " + veth(pid) +
        ": " + classifiers.error());
  } else if (classifiers.isNone()) {
    // Since we bind mount the network namespace handle (which causes
    // an extra reference), the veth should be present even if the
    // executor has exited. However, we may encounter a case where the
    // veth is removed (because the corresponding container is
    // destroyed), but the slave restarts before it is able to write
    // the sentinel file. In that case, when the slave restarts, it
    // will try to recover a container that has already been
    // destroyed. To distinguish this case, we return None here.
    return None();
  }

  IntervalSet<uint16_t> nonEphemeralPorts;
  IntervalSet<uint16_t> ephemeralPorts;

  foreach (const ip::Classifier& classifier, classifiers.get()) {
    Option<PortRange> sourcePorts = classifier.sourcePorts();
    Option<PortRange> destinationPorts = classifier.destinationPorts();

    // All the IP filters on veth used by us only have source ports.
    if (sourcePorts.isNone() || destinationPorts.isSome()) {
      return Error("Unexpected IP filter detected on " + veth(pid));
    }

    Interval<uint16_t> ports =
      (Bound<uint16_t>::closed(sourcePorts.get().begin()),
       Bound<uint16_t>::closed(sourcePorts.get().end()));

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

  if (ephemeralPorts.empty()) {
    return Error("No ephemeral ports found");
  }

  if (ephemeralPorts.intervalCount() != 1) {
    return Error("Each container should have only one ephemeral port range");
  }

  // Tell the allocator that this ephemeral port range is used.
  ephemeralPortsAllocator->allocate(*ephemeralPorts.begin());

  Info* info = new Info(nonEphemeralPorts, *ephemeralPorts.begin(), pid);
  CHECK_NOTNULL(info);

  VLOG(1) << "Recovered network isolator for container with pid " << pid
          << " non-ephemeral port ranges " << nonEphemeralPorts
          << " and ephemeral port range " << *ephemeralPorts.begin();

  return info;
}


Future<Option<CommandInfo> > PortMappingIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo)
{
  if (unmanaged.contains(containerId)) {
    return Failure("Asked to prepare an unmanaged container");
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  Resources resources(executorInfo.resources());

  IntervalSet<uint16_t> nonEphemeralPorts;

  if (resources.ports().isSome()) {
    nonEphemeralPorts = getIntervalSet(resources.ports().get());

    // Sanity check to make sure that the assigned non-ephemeral ports
    // for the container are part of the non-ephemeral ports specified
    // by the slave.
    if (!managedNonEphemeralPorts.contains(nonEphemeralPorts)) {
        return Failure(
            "Some non-ephemeral ports specified in " +
            stringify(nonEphemeralPorts) +
            " are not managed by the slave");
    }
  }

  // Determine the ephemeral ports used by this container.
  Try<Interval<uint16_t> > ephemeralPorts = ephemeralPortsAllocator->allocate();
  if (ephemeralPorts.isError()) {
    return Failure(
        "Failed to allocate ephemeral ports: " + ephemeralPorts.error());
  }

  infos[containerId] =
    CHECK_NOTNULL(new Info(nonEphemeralPorts, ephemeralPorts.get()));

  LOG(INFO) << "Allocated non-ephemeral ports " << nonEphemeralPorts
            << " and ephemeral ports " << ephemeralPorts.get()
            << " for container " << containerId << " of executor "
            << executorInfo.executor_id();

  CommandInfo command;
  command.set_value(scripts(infos[containerId]));

  return command;
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

  // Bind mount the network namespace handle of the process 'pid' to a
  // directory to hold an extra reference to the network namespace
  // which will be released in 'cleanup'. By holding the extra
  // reference, the network namespace will not be destroyed even if
  // the process 'pid' is gone, which allows us to explicitly control
  // the network namespace life cycle.
  const string source = path::join("/proc", stringify(pid), "ns", "net");
  const string target = path::join(BIND_MOUNT_ROOT, stringify(pid));

  Try<Nothing> touch = os::touch(target);
  if (touch.isError()) {
    return Failure("Failed to create the bind mount point: " + touch.error());
  }

  Try<Nothing> mount = fs::mount(source, target, "none", MS_BIND, NULL);
  if (mount.isError()) {
    return Failure(
        "Failed to mount the network namespace handle from " +
        source + " to " + target + ": " + mount.error());
  }

  LOG(INFO) << "Bind mounted " << source << " to " << target
            << " for container " << containerId;

  // Create a virtual ethernet pair for this container.
  Try<bool> createVethPair = link::create(veth(pid), eth0, pid);
  if (createVethPair.isError()) {
    return Failure(
        "Failed to create virtual ethernet pair: " +
        createVethPair.error());
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
    LOG(INFO) << "Adding IP packet filters with ports " << range
              << " for container " << containerId;

    Try<Nothing> add = addHostIPFilters(
        range,
        eth0,
        lo,
        veth(pid),
        hostMAC,
        hostIP);

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
    return Failure(
        "Failed to create an ICMP packet filter from " + veth(pid) +
        " to host " + eth0 + ": " + icmpVethToEth0.error());
  } else if (!icmpVethToEth0.get()) {
    return Failure(
        "The ICMP packet filter from " + veth(pid) +
        " to host " + eth0 + " already exists");
  }

  // Relay ARP packets from veth of the container to host eth0.
  Try<bool> arpVethToEth0 = filter::arp::create(
      veth(pid),
      ingress::HANDLE,
      Priority(ARP_FILTER_PRIORITY, NORMAL),
      action::Redirect(eth0));

  if (arpVethToEth0.isError()) {
    return Failure(
        "Failed to create an ARP packet filter from " + veth(pid) +
        " to host " + eth0 + ": " + arpVethToEth0.error());
  } else if (!arpVethToEth0.get()) {
    return Failure(
        "The ARP packet filter from " + veth(pid) +
        " to host " + eth0 + " already exists");
  }

  // Mirror ICMP and ARP packets from host eth0 to veths of all the
  // containers.
  set<string> targets;
  foreachvalue (Info* info, infos) {
    if (info->pid.isSome()) {
      targets.insert(veth(info->pid.get()));
    }
  }

  if (targets.size() == 1) {
    // Create a new ICMP filter on host eth0.
    Try<bool> icmpEth0ToVeth = filter::icmp::create(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(net::IP(hostIP.address())),
        Priority(ICMP_FILTER_PRIORITY, NORMAL),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      return Failure(
          "Failed to create an ICMP packet filter from host " + eth0 +
          " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      return Failure(
          "The ICMP packet filter on host " + eth0 + " already exists");
    }

    // Create a new ARP filter on host eth0.
    Try<bool> arpEth0ToVeth = filter::arp::create(
        eth0,
        ingress::HANDLE,
        Priority(ARP_FILTER_PRIORITY, NORMAL),
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      return Failure(
          "Failed to create an ARP packet filter from host " + eth0 +
          " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      return Failure(
          "The ARP packet filter on host " + eth0 + " already exists");
    }
  } else {
    // Update the ICMP filter on host eth0.
    Try<bool> icmpEth0ToVeth = filter::icmp::update(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(net::IP(hostIP.address())),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      return Failure(
          "Failed to append a ICMP mirror action from host " +
           eth0 + " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      return Failure(
          "The ICMP packet filter on host " + eth0 + " already exists");
    }

    // Update the ARP filter on host eth0.
    Try<bool> arpEth0ToVeth = filter::arp::update(
        eth0,
        ingress::HANDLE,
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      return Failure(
          "Failed to append an ARP mirror action from host " +
           eth0 + " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      return Failure(
          "The ARP packet filter on host " + eth0 + " already exists");
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


Future<Limitation> PortMappingIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (unmanaged.contains(containerId)) {
    LOG(WARNING) << "Ignoring watch for unmanaged container " << containerId;
  } else if (!infos.contains(containerId)) {
    LOG(WARNING) << "Ignoring watch for unknown container "  << containerId;
  }

  // Currently, we always return a pending future because limitation
  // is never reached.
  return Future<Limitation>();
}


static void _update(
    const Future<Option<int> >& status,
    const ContainerID& containerId)
{
  if (!status.isReady()) {
    LOG(ERROR) << "Failed to launch the launcher for updating container "
               << containerId << ": "
               << (status.isFailed() ? status.failure() : "discarded");
  } else if (status.get().isNone()) {
    LOG(ERROR) << "The launcher for updating container " << containerId
               << " is not expected to be reaped elsewhere";
  } else if (status.get().get() != 0) {
    LOG(ERROR) << "Received non-zero exit status " << status.get().get()
               << " from the launcher for updating container " << containerId;
  } else {
    LOG(INFO) << "The launcher for updating container " << containerId
              << " finished successfully";
  }
}


Future<Nothing> PortMappingIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
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

  Info* info = CHECK_NOTNULL(infos[containerId]);

  if (info->pid.isNone()) {
    return Failure("The container has not been isolated");
  }
  pid_t pid = info->pid.get();

  IntervalSet<uint16_t> nonEphemeralPorts;

  if (resources.ports().isSome()) {
    nonEphemeralPorts = getIntervalSet(resources.ports().get());

    // Sanity check to make sure that the assigned non-ephemeral ports
    // for the container are part of the non-ephemeral ports specified
    // by the slave.
    if (!managedNonEphemeralPorts.contains(nonEphemeralPorts)) {
        return Failure(
            "Some non-ephemeral ports specified in " +
            stringify(nonEphemeralPorts) +
            " are not managed by the slave");
    }
  }

  // No need to proceed if no change to the non-ephemeral ports.
  if (nonEphemeralPorts == info->nonEphemeralPorts) {
    return Nothing();
  }

  LOG(INFO) << "Updating non-ephemeral ports for container "
            << containerId << " from " << info->nonEphemeralPorts
            << " to " << nonEphemeralPorts;

  Result<vector<ip::Classifier> > classifiers =
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
    Option<PortRange> sourcePorts = classifier.sourcePorts();
    Option<PortRange> destinationPorts = classifier.destinationPorts();

    // All the IP filters on veth used by us only have source ports.
    if (sourcePorts.isNone() || destinationPorts.isSome()) {
      return Failure("Unexpected IP filter detected on " + veth(pid));
    }

    Interval<uint16_t> ports =
      (Bound<uint16_t>::closed(sourcePorts.get().begin()),
       Bound<uint16_t>::closed(sourcePorts.get().end()));

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
    LOG(INFO) << "Adding IP packet filters with ports " << range
              << " for container " << containerId;

    Try<Nothing> add = addHostIPFilters(
        range,
        eth0,
        lo,
        veth(pid),
        hostMAC,
        hostIP);

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

    Try<Nothing> removing =
      removeHostIPFilters(
          range,
          eth0,
          lo,
          veth(pid),
          hostMAC,
          hostIP);

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
      update.flags);

  if (s.isError()) {
    return Failure("Failed to launch update subcommand: " + s.error());
  }

  return s.get().status()
    .onAny(lambda::bind(&_update, lambda::_1, containerId))
    .then(lambda::bind(&_nothing));
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

  if(info->pid.isNone()) {
    return result;
  }

  Result<hashmap<string, uint64_t> > stat =
    link::statistics(veth(info->pid.get()));

  if (stat.isError()) {
    return Failure(
        "Failed to retrieve statistics on link " +
        veth(info->pid.get()) + ": " + stat.error());
  } else if (stat.isNone()) {
     return Failure(
         "Failed to find link: " + veth(info->pid.get()));
  }

  Option<uint64_t> rx_packets = stat.get().get("rx_packets");
  if (rx_packets.isSome()) {
    result.set_net_rx_packets(rx_packets.get());
  }

  Option<uint64_t> rx_bytes = stat.get().get("rx_bytes");
  if (rx_bytes.isSome()) {
    result.set_net_rx_bytes(rx_bytes.get());
  }

  Option<uint64_t> rx_errors = stat.get().get("rx_errors");
  if (rx_errors.isSome()) {
    result.set_net_rx_errors(rx_errors.get());
  }

  Option<uint64_t> rx_dropped = stat.get().get("rx_dropped");
  if (rx_dropped.isSome()) {
    result.set_net_rx_dropped(rx_dropped.get());
  }

  Option<uint64_t> tx_packets = stat.get().get("tx_packets");
  if (tx_packets.isSome()) {
    result.set_net_tx_packets(tx_packets.get());
  }

  Option<uint64_t> tx_bytes = stat.get().get("tx_bytes");
  if (tx_bytes.isSome()) {
    result.set_net_tx_bytes(tx_bytes.get());
  }

  Option<uint64_t> tx_errors = stat.get().get("tx_errors");
  if (tx_errors.isSome()) {
    result.set_net_tx_errors(tx_errors.get());
  }

  Option<uint64_t> tx_dropped = stat.get().get("tx_dropped");
  if (tx_dropped.isSome()) {
    result.set_net_tx_dropped(tx_dropped.get());
  }

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

  Try<Nothing> cleanup = _cleanup(info);
  if (cleanup.isError()) {
    return Failure(cleanup.error());
  }

  return Nothing();
}


// An old glibc might not have this symbol.
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


Try<Nothing> PortMappingIsolatorProcess::_cleanup(Info* _info)
{
  // Set '_info' to be auto-managed so that it will be deleted when
  // this function returns.
  Owned<Info> info(CHECK_NOTNULL(_info));

  if(!info->pid.isSome()) {
    LOG(WARNING) << "The container has not been isolated";
    return Nothing();
  }

  pid_t pid = info->pid.get();

  // Remove the IP filters on eth0 and lo for non-ephemeral port
  // ranges and the ephemeral port range.
  foreach (const PortRange& range,
           getPortRanges(info->nonEphemeralPorts + info->ephemeralPorts)) {
    LOG(INFO) << "Removing IP packet filters with ports " << range
              << " for container with pid " << pid;

    Try<Nothing> removing =
      removeHostIPFilters(
          range,
          eth0,
          lo,
          veth(pid),
          hostMAC,
          hostIP);

    if (removing.isError()) {
      return Error(
          "Failed to remove IP packet filter with ports " +
          stringify(range) + " for container with pid " +
          stringify(pid) + ": " + removing.error());
    }
  }

  // Free the ephemeral ports used by this container.
  ephemeralPortsAllocator->deallocate(info->ephemeralPorts);

  LOG(INFO) << "Freed ephemeral ports " << info->ephemeralPorts
            << " for container with pid " << pid;

  set<string> targets;
  foreachvalue (Info* info, infos) {
    if (info->pid.isSome()) {
      targets.insert(veth(info->pid.get()));
    }
  }

  if (targets.empty()) {
    // This is the last container, remove the ARP and ICMP filters on
    // host eth0.

    // Remove the ICMP filter on host eth0.
    Try<bool> icmpEth0ToVeth = filter::icmp::remove(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(net::IP(hostIP.address())));

    if (icmpEth0ToVeth.isError()) {
      return Error(
          "Failed to remove the ICMP packet filter on host " + eth0 +
          ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      LOG(ERROR) << "The ICMP packet filter on host " << eth0
                 << " does not exist";
    }

    // Remove the ARP filter on host eth0.
    Try<bool> arpEth0ToVeth = filter::arp::remove(
        eth0,
        ingress::HANDLE);

    if (arpEth0ToVeth.isError()) {
      return Error(
          "Failed to remove the ARP packet filter on host " + eth0 +
          ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      LOG(ERROR) << "The ARP packet filter on host " << eth0
                 << " does not exist";
    }

  } else {
    // This is not the last container. Replace the ARP and ICMP
    // filters. The reason we do this is that we don't have an easy
    // way to search and delete an action from the multiple actions on
    // a single filter.
    Try<bool> icmpEth0ToVeth = filter::icmp::update(
        eth0,
        ingress::HANDLE,
        icmp::Classifier(net::IP(hostIP.address())),
        action::Mirror(targets));

    if (icmpEth0ToVeth.isError()) {
      return Error(
          "Failed to update the ICMP mirror action from host " + eth0 +
          " to " + veth(pid) + ": " + icmpEth0ToVeth.error());
    } else if (!icmpEth0ToVeth.get()) {
      return Error(
          "The ICMP packet filter on host " + eth0 + " does not exist");
    }

    Try<bool> arpEth0ToVeth = filter::arp::update(
        eth0,
        ingress::HANDLE,
        action::Mirror(targets));

    if (arpEth0ToVeth.isError()) {
      return Error(
          "Failed to update the ARP mirror action from host " + eth0 +
          " to " + veth(pid) + ": " + arpEth0ToVeth.error());
    } else if (!arpEth0ToVeth.get()) {
      return Error(
          "The ARP packet filter on host " + eth0 + " does not exist");
    }
  }

  // Release the bind mount for this container.
  const string target = path::join(BIND_MOUNT_ROOT, stringify(pid));

  Try<Nothing> unmount = fs::unmount(target, MNT_DETACH);
  if (unmount.isError()) {
    return Error("Failed to umount: " + unmount.error());
  }

  Try<Nothing> rm = os::rm(target);
  if (rm.isError()) {
    return Error("Failed to remove " + target + ": " + rm.error());
  }

  // We manually remove veth to avoid having to wait for the kernel to
  // do it.
  Try<bool> remove = link::remove(veth(pid));
  if (remove.isError()) {
    return Error(
        "Failed to remove the link " + veth(pid) + ": " + remove.error());
  }

  LOG(INFO) << "Successfully removed the link " << veth(pid);

  return Nothing();
}

// This function returns the scripts that need to be run in child
// context before child execs to complete network isolation.
// TODO(jieyu): Use the launcher abstraction to remove most of the
// logic here. Completely remove this function once we can assume a
// newer kernel where 'setns' works for mount namespaces.
string PortMappingIsolatorProcess::scripts(Info* info)
{
  ostringstream script;

  script << "#!/bin/sh\n";
  script << "set -x\n";

  // Remount /proc and /sys to show a separate networking stack.
  // These should be done by a FilesystemIsolator in the future.
  script << "mount -n -o remount -t sysfs none /sys\n";
  script << "mount -n -o remount -t proc none /proc\n";

  // Mark the mount point BIND_MOUNT_ROOT as slave mount so that
  // changes in the container will not be propagated to the host.
  script << "mount --make-rslave " << BIND_MOUNT_ROOT << "\n";

  // Configure lo and eth0.
  script << "ip link set " << lo << " address " << hostMAC
         << " mtu "<< hostEth0MTU << " up\n";

  script << "ip link set " << eth0 << " address " << hostMAC << " up\n";
  script << "ip addr add " << hostIP  << " dev " << eth0 << "\n";

  // Set up the default gateway to match that of eth0.
  script << "ip route add default via "
         << net::IP(hostDefaultGateway.address()) << "\n";

  // Restrict the ephemeral ports that can be used by the container.
  script << "echo -e " << info->ephemeralPorts.lower() << "\t"
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

  // Set up filters on lo and eth0.
  script << "tc qdisc add dev " << lo << " ingress\n";
  script << "tc qdisc add dev " << eth0 << " ingress\n";

  // Allow talking between containers and from container to host.
  // TODO(chzhcn): Consider merging the following two filters.
  script << "tc filter add dev " << lo << " parent ffff: protocol ip"
         << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip dst " << net::IP(hostIP.address())
         << " action mirred egress redirect dev " << eth0 << "\n";

  script << "tc filter add dev " << lo << " parent ffff: protocol ip"
         << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip dst " << net::IP(LOOPBACK_IP.address())
         << " action mirred egress redirect dev " << eth0 << "\n";

  foreach (const PortRange& range,
           getPortRanges(info->nonEphemeralPorts + info->ephemeralPorts)) {
    // Local traffic inside a container will not be redirected to eth0.
    script << "tc filter add dev " << lo << " parent ffff: protocol ip"
           << " prio " << Priority(IP_FILTER_PRIORITY, HIGH).get() << " u32"
           << " flowid ffff:0"
           << " match ip dport " << range.begin() << " "
           << hex << range.mask() << dec << "\n";

    // Traffic going to host loopback IP and ports assigned to this
    // container will be redirected to lo.
    script << "tc filter add dev " << eth0 << " parent ffff: protocol ip"
           << " prio " << Priority(IP_FILTER_PRIORITY, NORMAL).get() << " u32"
           << " flowid ffff:0"
           << " match ip dst " << net::IP(LOOPBACK_IP.address())
           << " match ip dport " << range.begin() << " "
           << hex << range.mask() << dec
           << " action mirred egress redirect dev " << lo << "\n";
  }

  // Do not forward the ICMP packet if the destination IP is self.
  script << "tc filter add dev " << lo << " parent ffff: protocol ip"
         << " prio " << Priority(ICMP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip protocol 1 0xff"
         << " match ip dst " << net::IP(hostIP.address()) << "\n";

  script << "tc filter add dev " << lo << " parent ffff: protocol ip"
         << " prio " << Priority(ICMP_FILTER_PRIORITY, NORMAL).get() << " u32"
         << " flowid ffff:0"
         << " match ip protocol 1 0xff"
         << " match ip dst " << net::IP(LOOPBACK_IP.address()) << "\n";

  // Display the filters created on eth0 and lo.
  script << "tc filter show dev " << eth0 << " parent ffff:\n";
  script << "tc filter show dev " << lo << " parent ffff:\n";

  return script.str();
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


Try<Interval<uint16_t> > EphemeralPortsAllocator::allocate()
{
  if (portsPerContainer_ == 0) {
    return Error("Number of ephemeral ports per container is zero");
  }

  Option<Interval<uint16_t> > allocated;

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
