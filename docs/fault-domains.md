---
title: Apache Mesos - Domains and Regions
layout: documentation
---

# Regions and Fault Domains

Starting with Mesos 1.5, it is possible to place Mesos masters and agents into
*domains*, which are logical groups of machines that share some characteristics.

Currently, fault domains are the only supported type of domains, which are
groups of machines with similar failure characteristics.

A fault domain is a 2 level hierarchy of regions and zones. The mapping from
fault domains to physical infrastructure is up to the operator to configure,
although it is recommended that machines in the same zones have low latency to
each other.

In cloud environments, regions and zones can be mapped to the "region" and
"availability zone" concepts exposed by most cloud providers, respectively.
In on-premise deployments, regions and zones can be mapped to data centers and
racks, respectively.

Schedulers may prefer to place network-intensive workloads in the same domain,
as this may improve performance. Conversely, a single failure that affects a
host in a domain may be more likely to affect other hosts in the same domain;
hence, schedulers may prefer to place workloads that require high availability
in multiple domains. For example, all the hosts in a single rack might lose
power or network connectivity simultaneously.

The `--domain` flag can be used to specify the fault domain of a master or
agent node. The value of this flag must be a file path or a JSON dictionary
with the key `fault_domain` and subkeys `region` and `zone` mapping to
arbitrary strings:

    mesos-master --domain='{"fault_domain": {"region": {"name":"eu"}, "zone": { "name":"rack1"}}}'

    mesos-agent  --domain='{"fault_domain": {"region": {"name":"eu"}, "zone": {"name":"rack2"}}}'

Frameworks can learn about the domain of an agent by inspecting the `domain`
field in the received offer, which contains a `DomainInfo` that has the
same structure as the JSON dictionary above.


# Constraints

When configuring fault domains for the masters and agents, the following
constraints must be obeyed:

 * If a mesos master is not configured with a domain, it will reject connection
   attempts from agents with a domain.

   This is done because the master is not able to determine whether or not the
   agent would be remote in this case.

 * Agents with no configured domain are assumed to be in the same domain as the
   master.

   If this behaviour isn't desired, the `--require_agent_domain` flag on the
   master can be used to enforce that domains are configured on all agents by
   having the master reject all registration attempts by agents without a
   configured domain.

 * If one master is configured with a domain, all other masters must be in the
   same "region" to avoid cross-region quorum writes. It is recommended to put
   them in different zones within that region for high availability.

 * The default DRF resource allocator will only offer resources from agents in
   the same region as the master. To receive offers from all regions, a
   framework must set the `REGION_AWARE` capability bit in its FrameworkInfo.


# Example

A short example will serve to illustrate these concepts. WayForward Technologies
runs a successful website that allows users to purchase things that they want
to have.

To do this, it owns a data center in San Francisco, in which it runs a number of
custom Mesos frameworks. All agents within the data center are configured with
the same region `sf`, and the individual racks inside the data center are used
as zones.

The three mesos masters are placed in different server racks in the data center,
which gives them enough isolation to withstand events like a whole rack losing
power or network connectivity but still have low-enough latency for
quorum writes.

One of the provided services is a real-time view of the company's inventory.
The framework providing this service is placing all of its tasks in the same
zone as the database server, to take advantage of the high-speed, low-latency
link so it can always display the latest results.

During peak hours, it might happen that the computing power required to operate
the website exceeds the capacity of the data center. To avoid unnecessary
hardware purchases, WayForward Technologies contracted with a third-party cloud
provider TPC. The machines from this provider are placed in a different
region `tpc`, and the zones are configured to correspond to the availability
zones provided by TPC. All relevant frameworks are updated with the
`REGION_AWARE` bit in their `FrameworkInfo` and their scheduling logic is
updated so that they can schedule tasks in the cloud if required.

Non-region aware frameworks will now only receive offers from agents within
the data center, where the master nodes reside. Region-aware frameworks are
supposed to know when and if they should place their tasks in the data center
or with the cloud provider.
