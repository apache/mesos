---
layout: documentation
---

# Network Monitoring

Mesos 0.20.0 adds the support for per container network monitoring. Network statistics for each active container can be retrieved through the `/monitor/statistics.json` endpoint on the slave.

The current solution is completely transparent to the tasks running on the slave. In other words, tasks will not notice any difference as if they were running on a slave without network monitoring turned on and were sharing the network of the slave.

## How to setup?

To turn on network monitoring on your mesos cluster, you need to follow the following procedures.

### Prerequisites

Currently, network monitoring is only supported on Linux. Make sure your kernel is at least 3.6. Also, check your kernel to make sure that the following upstream patches are merged in (Mesos will automatically check for those kernel functionalities and will abort if they are not supported):

* [6a662719c9868b3d6c7d26b3a085f0cd3cc15e64](https://github.com/torvalds/linux/commit/6a662719c9868b3d6c7d26b3a085f0cd3cc15e64)
* [0d5edc68739f1c1e0519acbea1d3f0c1882a15d7](https://github.com/torvalds/linux/commit/0d5edc68739f1c1e0519acbea1d3f0c1882a15d7)
* [e374c618b1465f0292047a9f4c244bd71ab5f1f0](https://github.com/torvalds/linux/commit/e374c618b1465f0292047a9f4c244bd71ab5f1f0)
* [25f929fbff0d1bcebf2e92656d33025cd330cbf8](https://github.com/torvalds/linux/commit/25f929fbff0d1bcebf2e92656d33025cd330cbf8)

Make sure the following packages are installed on the slave:

* [libnl3](http://www.infradead.org/~tgr/libnl/) >= 3.2.26
* [iproute](http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2) (>= 2.6.39 is advised but not required for debugging purpose)

On the build machine, you need to install the following packages:

* [libnl3-devel](http://www.infradead.org/~tgr/libnl/) >= 3.2.26

### Configure and build

Network monitoring will NOT be built in by default. To build Mesos with network monitoring support, you need to add a configure option:

    $ ./configure --with-network-isolator
    $ make


### Host ephemeral ports squeeze

With network monitoring being turned on, each container on the slave will have a separate network stack (via Linux [network namespaces](http://lwn.net/Articles/580893/)). All containers share the same public IP of the slave (so that service discovery mechanism does not need to be changed). Each container will be assigned a subset of the ports from the host, and is only allowed to use those ports to make connections with other hosts.

For non-ephemeral ports (e.g, listening ports), Mesos already exposes that to the scheduler (resource: 'ports'). The scheduler is responsible for allocating those ports to executors/tasks.

For ephemeral ports, without network monitoring, all executors/tasks running on the slave share the same ephemeral port range of the host. The default ephemeral port range on most Linux distributions is [32768, 61000]. With network monitoring, for each container, we need to reserve a range for ports on the host which will be used as the ephemeral port range for the container network stack (these ports are directly mapped into the container). We need to ensure none of the host processes are using those ports. Because of that, you may want to squeeze the host ephemeral port range in order to support more containers on each slave. To do that, you can use the following command (need root permission). A host reboot is required to ensure there are no connections using ports outside the new ephemeral range.

    # This sets the host ephemeral port range to [57345, 61000].
    $ echo "57345 61000" > /proc/sys/net/ipv4/ip_local_port_range


### Turn on network monitoring

After the host ephemeral ports squeeze and reboot, you can turn on network monitoring by appending `network/port_mapping` to the isolation flag. Notice that you need specify the `ephemeral_ports` resource (via --resources flag). It tells the slave which ports on the host are reserved for containers. It must NOT overlap with the host ephemeral port range. You can also specify how many ephemeral ports you want to allocate to each container. It is recommended but not required that this number is power of 2 aligned (e.g., 512, 1024). If not, there will be some performance impact for classifying packets. The maximum number of containers on the slave will be limited by approximately |ephemeral_ports|/ephemeral_ports_per_container, subject to alignment etc.

    mesos-slave \
        --checkpoint \
        --log_dir=/var/log/mesos \
        --work_dir=/var/lib/mesos \
        --isolation=cgroups/cpu,cgroups/mem,network/port_mapping \
        --resources=cpus:22;mem:62189;ports:[31000-32000];disk:400000;ephemeral_ports:[32768-57344] \
        --ephemeral_ports_per_container=1024


## How to get statistics?

Currently, we report the following network statistics:

* _net_rx_bytes_
* _net_rx_dropped_
* _net_rx_errors_
* _net_rx_packets_
* _net_tx_bytes_
* _net_tx_dropped_
* _net_tx_errors_
* _net_tx_packets_

For example, these are the statistics you will get by hitting the `/monitor/statistics.json` endpoint on a slave with network monitoring turned on:

    $ curl -s http://localhost:5051/monitor/statistics.json | python2.6
    -mjson.tool
    [
        {
            "executor_id": "sample_executor_id-ebd8fa62-757d-489e-9e23-678a21d078d6",
            "executor_name": "sample_executor",
            "framework_id": "201103282247-0000000019-0000",
            "source": "sample_executor",
            "statistics": {
                "cpus_limit": 0.35,
                "cpus_nr_periods": 520883,
                "cpus_nr_throttled": 2163,
                "cpus_system_time_secs": 154.42,
                "cpus_throttled_time_secs": 145.96,
                "cpus_user_time_secs": 258.74,
                "mem_anon_bytes": 109137920,
                "mem_file_bytes": 30613504,
                "mem_limit_bytes": 167772160,
                "mem_mapped_file_bytes": 8192,
                "mem_rss_bytes": 140341248,
                "net_rx_bytes": 2402099,
                "net_rx_dropped": 0,
                "net_rx_errors": 0,
                "net_rx_packets": 33273,
                "net_tx_bytes": 1507798,
                "net_tx_dropped": 0,
                "net_tx_errors": 0,
                "net_tx_packets": 17726,
                "timestamp": 1408043826.91626
            }
        }
    ]


# Network Egress Rate Limit

Mesos 0.21.0 adds an optional feature to limit the egress network bandwidth for each container. With this feature enabled, each container's egress traffic is limited to the specified rate. This can prevent a single container from dominating the entire network.

## How to enable it?

Egress Rate Limit requires Network Monitoring. To enable it, please follow all the steps in the [previous section](#Network_Monitoring) to enable the Network Monitoring first, and then use the newly introduced `egress_rate_limit_per_container` flag to specify the rate limit for each container. Note that this flag expects a `Bytes` type like the following:

    mesos-slave \
        --checkpoint \
        --log_dir=/var/log/mesos \
        --work_dir=/var/lib/mesos \
        --isolation=cgroups/cpu,cgroups/mem,network/port_mapping \
        --resources=cpus:22;mem:62189;ports:[31000-32000];disk:400000;ephemeral_ports:[32768-57344] \
        --ephemeral_ports_per_container=1024 \
        --egress_rate_limit_per_container=37500KB # Convert to ~300Mbits/s.
