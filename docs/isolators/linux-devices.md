---
title: Apache Mesos - Linux Devices Isolator in Mesos Containerizer
layout: documentation
---

# Linux Devices in Mesos Containerizer

While the `cgroups/devices` isolator allows operators to control
container access to host devices, the container might still need
additional privileges to create a device node to actually use the
device. The `linux/devices` isolator ensures that containers that
are granted access to host devices are populated with the the correct
set of device nodes. Access to host devices is granted by using the
[`--allowed_devices`](../configuration/agent.md#allowed_devices) flag
on the agent.

To enable the `linux/devices` isolator, append `linux/devices` to the
[`--isolation`](../configuration/agent.md#isolation) flag when starting
the Mesos agent.

## Security Considerations

Device access is configured at container
granularity. For example, this means that if the
[`--allowed_devices`](../configuration/agent.md#allowed_devices) flag
specifies read access for a device, then every process in the container
will be able to read from the specified device.

The `linux/devices` isolator does not require the
[`--allowed_devices`](../configuration/agent.md#allowed_devices) entry
to grant `mknod` access, since it creates device nodes from outside
the container.
