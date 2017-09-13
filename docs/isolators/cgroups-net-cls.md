---
title: Apache Mesos - Cgroups 'net_cls' Subsystem Support in Mesos Containerizer
layout: documentation
---

# Cgroups 'net_cls' Subsystem Support in Mesos Containerizer

The `cgroups/net_cls` isolator allows operators to provide network
performance isolation and network segmentation for containers within a
Mesos cluster. To enable the `cgroups/net_cls` isolator, append
`cgroups/net_cls` to the `--isolation` flag when starting the agent.

As the name suggests, the isolator enables the `net_cls` subsystem for
Linux cgroups and assigns a `net_cls` cgroup to each container
launched by the Mesos Containerizer. The objective of the `net_cls`
subsystem is to allow the kernel to tag packets originating from a
container with a 32-bit handle. These handles can be used by kernel
modules such as `qdisc` (for traffic engineering) and `net-filter`
(for firewall) to enforce network performance and security policies
specified by the operators.  The policies, based on the `net_cls`
handles, can be specified by the operators through user-space tools
such as
[tc](http://tldp.org/HOWTO/Traffic-Control-HOWTO/software.html#s-iproute2-tc)
and [iptables](http://linux.die.net/man/8/iptables).

The 32-bit handle associated with a `net_cls` cgroup can be specified
by writing the handle to the `net_cls.classid` file, present within
the `net_cls` cgroup. The 32-bit handle is of the form `0xAAAABBBB`,
and consists of a 16-bit primary handle 0xAAAA and a 16-bit secondary
handle 0xBBBB. You can read more about the use cases for the primary
and secondary handles in the [Linux kernel documentation for
net_cls](https://www.kernel.org/doc/Documentation/cgroup-v1/net_cls.txt).

By default, the `cgroups/net_cls` isolator does not manage the
`net_cls` handles, and assumes the operator is going to manage/assign
these handles. To enable the management of `net_cls` handles by the
`cgroups/net_cls` isolator you need to specify a 16-bit primary
handle, of the form 0xAAAA, using the
`--cgroups_net_cls_primary_handle` flag at agent startup.

Once a primary handle has been specified for an agent, for each
container the `cgroups/net_cls` isolator allocates a 16-bit secondary
handle. It then assigns the 32-bit combination of the primary and
secondary handle to the `net_cls` cgroup associated with the container
by writing to `net_cls.classid`. The `cgroups/net_cls` isolator
exposes the assigned `net_cls` handle to operators by exposing the
handle as part of the `ContainerStatus` &mdash;associated with any
task running within the container&mdash; in the agent's
[/state](../endpoints/slave/state.md) endpoint.
