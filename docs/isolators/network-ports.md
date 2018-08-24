---
title: Apache Mesos - Network Ports Isolator in Mesos Containerizer
layout: documentation
---

# Network Ports Isolator in Mesos Containerizer

When tasks run in the host network namespace, a scheduler typically
assigns ports resources to them and they must only listen on the network
ports they have been assigned. If a task listens on the wrong ports,
the resulting failures can be hard to diagnose.

The network ports isolator enforces task port assignments by periodically
scanning the set of ports every task is listening on and reconciling that
information against the corresponding ports resources. If a task is found
to be listening on a port that has not been allocated to it, the task
will be killed and the its framework will receive a status update with
the reason `REASON_CONTAINER_LIMITATION`. The port(s) that triggered the
limitation will be reported as resources in the status update. If the task
is a member of a [task group](../nested-container-and-task-group.md),
the limitation is raised against the root of the task group (i.e. the
executor's container). This behavior is consistent across all the Mesos
resource isolators.

## Installation

The network ports isolator is not compiled into Mesos by default. To
enable it in build, specify the `--enable-network-ports-isolator`
configuration option.

[libnl3](https://github.com/thom311/libnl/releases) version 3.2.26 or
higher is required at both build and deployment time.

## Configuration

To enable the network ports isolator, append `network/ports` to the
`--isolation` flag when starting the agent.

The network ports isolator requires that the Mesos agent is configured
to use the Linux launcher (i.e. the agent has the `--launcher=linux`
flag), because it uses Linux cgroups to track the processes belonging
to a Mesos task.

The `--enforce_container_ports` flag, specifies whether the network
ports isolator should terminate tasks that listen on ports they have
not been assigned. If enforcement is disabled, the isolator will log
violations but will not terminate tasks. By default, network port
enforcement is disabled.

If the `--check_agent_port_range_only` flag is specified, the isolator
will not kill tasks that listen on unallocated ports outside the range
of port resources the agent offers to tasks. This flag is required when
using the default Mesos executors or any custom executor that uses the
native Mesos Java or Python bindings since the native Mesos libraries
will always implicity listen on a socket. This flag should not be
required for custom executors that use the HTTP executor API. This flag
can't be used in conjunction with `--container_ports_isolated_range`.

The `--container_ports_isolated_range` works just like the
`--check_agent_port_range_only` flag except that it accepts an
arbitrary range of ports. When this range flag is set, the isolator
will neither log nor terminate the task if the used unallocated
ports that are outside this range. This flag can't be used in
conjunction with `--check_agent_port_range_only`.

The `--container_ports_watch_interval` flag specifies the interval
between task port reconciliations.

The network ports isolator ignores tasks that belong to a [CNI](../cni.md)
network since these tasks do not share the host network namespace.

## Security Considerations

The network ports isolator is not a secure mechanism for enforcing
port assignments. Since it periodically reconciles port assignments,
it is possible for a sufficiently malicious task to only listen
on unassigned ports between reconciliations. The reconciliation is
performed by the Mesos agent, so if the agent has been stopped, no
port assignments are enforced. Depending on the configuration, the
`--check_agent_port_range_only` flag could allow a malicious task to
intercept network requests.
