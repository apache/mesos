---
title: Apache Mesos - Port Mapping Network Isolator
layout: documentation
---

# Port Mapping Network Isolator

The port mapping network isolator provides a way to achieve
per-container network monitoring and isolation without relying on IP
per container.  The network isolator prevents a single container from
exhausting the available network ports, consuming an unfair share of
the network bandwidth or significantly delaying packet transmission
for others. Network statistics for each active container are published
through the
[/monitor/statistics](../endpoints/slave/monitor/statistics.md)
endpoint on the agent. The port mapping network isolator is
transparent for the majority of tasks running on an agent (those that
bind to port 0 and let the kernel allocate their port).

## Installation

Port mapping network isolator is __not__ supported by default.  To
enable it you need to install additional dependencies and configure it
during the build process.

### Prerequisites

Per-container network monitoring and isolation is only supported on Linux kernel
versions 3.6 and above. Additionally, the kernel must include these patches
(merged in kernel version 3.15).

* [6a662719c9868b3d6c7d26b3a085f0cd3cc15e64](https://github.com/torvalds/linux/commit/6a662719c9868b3d6c7d26b3a085f0cd3cc15e64)
* [0d5edc68739f1c1e0519acbea1d3f0c1882a15d7](https://github.com/torvalds/linux/commit/0d5edc68739f1c1e0519acbea1d3f0c1882a15d7)
* [e374c618b1465f0292047a9f4c244bd71ab5f1f0](https://github.com/torvalds/linux/commit/e374c618b1465f0292047a9f4c244bd71ab5f1f0)
* [25f929fbff0d1bcebf2e92656d33025cd330cbf8](https://github.com/torvalds/linux/commit/25f929fbff0d1bcebf2e92656d33025cd330cbf8)

The following packages are required on the agent:

* [libnl3](https://github.com/thom311/libnl/releases) >= 3.2.26
* [iproute](http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2) >= 2.6.39 is advised for debugging purpose but not required.

Additionally, if you are building from source, you need will also need the
libnl3 development package to compile Mesos:

* [libnl3-devel / libnl3-dev](https://github.com/thom311/libnl/releases) >= 3.2.26

### Build

To build Mesos with port mapping network isolator support, you need to
add a configure option:

    $ ./configure --with-network-isolator
    $ make

## Configuration

The port mapping network isolator is enabled on the agent by adding
`network/port_mapping` to the agent command line `--isolation` flag.

    --isolation="network/port_mapping"

If the agent has not been compiled with port mapping network isolator
support, it will refuse to start and print an error:

    I0708 00:17:08.080271 44267 containerizer.cpp:111] Using isolation: network/port_mapping
    Failed to create a containerizer: Could not create MesosContainerizer: Unknown or unsupported
        isolator: network/port_mapping

## Configuring network ports

Without port mapping network isolator, all the containers on a host
share the public IP address of the agent and can bind to any port
allowed by the OS.

When the port mapping network isolator is enabled, each container on
the agent has a separate network stack (via Linux [network
namespaces](http://lwn.net/Articles/580893/)).  All containers still
share the same public IP of the agent (so that the service discovery
mechanism does not need to be changed). The agent assigns each
container a non-overlapping range of the ports and only packets
to/from these assigned port ranges will be delivered. Applications
requesting the kernel assign a port (by binding to port 0) will be
given ports from the container assigned range. Applications can bind
to ports outside the container assigned ranges but packets from
to/from these ports will be silently dropped by the host.

Mesos provides two ranges of ports to containers:

+ OS allocated "[ephemeral](https://en.wikipedia.org/wiki/Ephemeral_port)" ports
are assigned by the OS in a range specified for each container by Mesos.

+ Mesos allocated "non-ephemeral" ports are acquired by a framework using the
same Mesos resource offer mechanism used for cpu, memory etc. for allocation to
executors/tasks as required.

Additionally, the host itself will require ephemeral ports for network
communication. You need to configure these three __non-overlapping__ port ranges
on the host.

### Host ephemeral port range

The currently configured host ephemeral port range can be discovered at any time
using the command `sysctl net.ipv4.ip_local_port_range`. If ports need to be set
aside for agent containers, the ephemeral port range can be updated in
`/etc/sysctl.conf`. Rebooting after the update will apply the change and
eliminate the possibility that ports are already in use by other processes. For
example, by adding the following:

    # net.ipv4.ip_local_port_range defines the host ephemeral port range, by
    # default 32768-61000.  We reduce this range to allow the Mesos agent to
    # allocate ports 32768-57344
    # net.ipv4.ip_local_port_range = 32768 61000
    net.ipv4.ip_local_port_range = 57345 61000

### Container port ranges

The container ephemeral and non-ephemeral port ranges are configured using the
agent `--resources` flag. The non-ephemeral port range is provided to the
master, which will then offer it to frameworks for allocation.

The ephemeral port range is sub-divided by the agent, giving
`ephemeral_ports_per_container` (default 1024) to each container. The maximum
number of containers on the agent will therefore be limited to approximately:

    number of ephemeral_ports / ephemeral_ports_per_container

The master `--max_executors_per_agent` flag is be used to prevent allocation of
more executors on an agent when the ephemeral port range has been exhausted.

It is recommended (but not required) that `ephemeral_ports_per_container` be set
to a power of 2 (e.g., 512, 1024) and the lower bound of the ephemeral port
range be a multiple of `ephemeral_ports_per_container` to minimize CPU overhead
in packet processing. For example:

    --resources=ports:[31000-32000];ephemeral_ports:[32768-57344] \
    --ephemeral_ports_per_container=512

### Rate limiting container traffic

Outbound traffic from a container to the network can be rate limited to prevent
a single container from consuming all available network resources with
detrimental effects to the other containers on the host. The
`--egress_rate_limit_per_container` flag specifies that each container launched
on the host be limited to the specified bandwidth (in bytes per second).
Network traffic which would cause this limit to be exceeded is delayed for later
transmission. The TCP protocol will adjust to the increased latency and reduce
the transmission rate ensuring no packets need be dropped.

    --egress_rate_limit_per_container=100MB

We do not rate limit inbound traffic since we can only modify the network flows
after they have been received by the host and any congestion has already
occurred.

### Egress traffic isolation

Delaying network data for later transmission can increase latency and jitter
(variability) for all traffic on the interface. Mesos can reduce the impact on
other containers on the same host by using flow classification and isolation
using the containers port ranges to maintain unique flows for each container and
sending traffic from these flows fairly (using the
[FQ_Codel](https://tools.ietf.org/html/draft-hoeiland-joergensen-aqm-fq-codel-00)
algorithm). Use the `--egress_unique_flow_per_container` flag to enable.

    --egress_unique_flow_per_container

### Putting it all together

A complete agent command line enabling port mapping network isolator,
reserving ports 57345-61000 for host ephemeral ports, 32768-57344 for
container ephemeral ports, 31000-32000 for non-ephemeral ports
allocated by the framework, limiting container transmit bandwidth to
300 Mbits/second (37.5MBytes) with unique flows enabled would thus be:

    mesos-agent \
    --isolation=network/port_mapping \
    --resources=ports:[31000-32000];ephemeral_ports:[32768-57344] \
    --ephemeral_ports_per_container=1024 \
    --egress_rate_limit_per_container=37500KB \
    --egress_unique_flow_per_container

## Monitoring container network statistics

Mesos exposes statistics from the Linux network stack for each container network
on the [/monitor/statistics](../endpoints/slave/monitor/statistics.md) agent endpoint.

From the network interface inside the container, we report the following
counters (since container creation) under the `statistics` key:

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td><code>net_rx_bytes</code></td>
  <td>Received bytes</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_rx_dropped</code></td>
  <td>Packets dropped on receive</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_rx_errors</code></td>
  <td>Errors reported on receive</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_rx_packets</code></td>
  <td>Packets received</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_tx_bytes</code></td>
  <td>Sent bytes</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_tx_dropped</code></td>
  <td>Packets dropped on send</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_tx_errors</code></td>
  <td>Errors reported on send</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>net_tx_packets</code></td>
  <td>Packets sent</td>
  <td>Counter</td>
</tr>
</table>

Additionally, [Linux Traffic Control](
http://tldp.org/HOWTO/Traffic-Control-HOWTO/intro.html) can report the following
statistics for the elements which implement bandwidth limiting and bloat
reduction under the `statistics/net_traffic_control_statistics` key. The entry
for each of these elements includes:

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td><code>backlog</code></td>
  <td>Bytes queued for transmission [1]</td>
  <td>Gauge</td>
</tr>
<tr>
  <td><code>bytes</code></td>
  <td>Sent bytes</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>drops</code></td>
  <td>Packets dropped on send</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>overlimits</code></td>
  <td>Count of times the interface was over its transmit limit when it attempted to send a packet.  Since the normal action when the network is overlimit is to delay the packet, the overlimit counter can be incremented many times for each packet sent on a heavily congested interface. [2]</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>packets</code></td>
  <td>Packets sent</td>
  <td>Counter</td>
</tr>
<tr>
  <td><code>qlen</code></td>
  <td>Packets queued for transmission</td>
  <td>Gauge</td>
</tr>
<tr>
  <td><code>ratebps</code></td>
  <td>Transmit rate in bytes/second [3]</td>
  <td>Gauge</td>
</tr>
<tr>
  <td><code>ratepps</code></td>
  <td>Transmit rate in packets/second [3]</td>
  <td>Gauge</td>
</tr>
<tr>
  <td><code>requeues</code></td>
  <td>Packets failed to send due to resource contention (such as kernel locking) [3]</td>
  <td>Counter</td>
</tr>
</table>

[1] `backlog` is only reported on the bloat_reduction interface.

[2] `overlimits` are only reported on the bw_limit interface.

[3] Currently always reported as 0 by the underlying Traffic Control element.

For example, these are the statistics you will get by hitting the `/monitor/statistics` endpoint on an agent with network monitoring turned on:

    $ curl -s http://localhost:5051/monitor/statistics | python2.6 -mjson.tool
    [
        {
            "executor_id": "job.1436298853",
            "executor_name": "Command Executor (Task: job.1436298853) (Command: sh -c 'iperf ....')",
            "framework_id": "20150707-195256-1740121354-5150-29801-0000",
            "source": "job.1436298853",
            "statistics": {
                "cpus_limit": 1.1,
                "cpus_nr_periods": 16314,
                "cpus_nr_throttled": 16313,
                "cpus_system_time_secs": 2667.06,
                "cpus_throttled_time_secs": 8036.840845388,
                "cpus_user_time_secs": 123.49,
                "mem_anon_bytes": 8388608,
                "mem_cache_bytes": 16384,
                "mem_critical_pressure_counter": 0,
                "mem_file_bytes": 16384,
                "mem_limit_bytes": 167772160,
                "mem_low_pressure_counter": 0,
                "mem_mapped_file_bytes": 0,
                "mem_medium_pressure_counter": 0,
                "mem_rss_bytes": 8388608,
                "mem_total_bytes": 9945088,
                "net_rx_bytes": 10847,
                "net_rx_dropped": 0,
                "net_rx_errors": 0,
                "net_rx_packets": 143,
                "net_traffic_control_statistics": [
                    {
                        "backlog": 0,
                        "bytes": 163206809152,
                        "drops": 77147,
                        "id": "bw_limit",
                        "overlimits": 210693719,
                        "packets": 107941027,
                        "qlen": 10236,
                        "ratebps": 0,
                        "ratepps": 0,
                        "requeues": 0
                    },
                    {
                        "backlog": 15481368,
                        "bytes": 163206874168,
                        "drops": 27081494,
                        "id": "bloat_reduction",
                        "overlimits": 0,
                        "packets": 107941070,
                        "qlen": 10239,
                        "ratebps": 0,
                        "ratepps": 0,
                        "requeues": 0
                    }
                ],
                "net_tx_bytes": 163200529816,
                "net_tx_dropped": 0,
                "net_tx_errors": 0,
                "net_tx_packets": 107936874,
                "perf": {
                    "duration": 0,
                    "timestamp": 1436298855.82807
                },
                "timestamp": 1436300487.41595
            }
        }
    ]
