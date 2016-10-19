# Linux Capabilities Support in Mesos Containerizer

This document describes the `linux/capabilities` isolator. The
isolator adds support for controlling [Linux
Capabilities](http://man7.org/linux/man-pages/man7/capabilities.7.html)
of containers launched using the
[MesosContainerizer](mesos-containerizer.md)

The Linux capabilities isolator allows operators to control which
privileged operations Mesos tasks may perform. Operators can specify
which capabilities to allow for containers executing on an agent;
containers on the other hand can expose which capabilities they need.

See the protobuf definition of `CapabilityInfo::Capability` for the
list of currently exposed capabilities.


## Agent setup

The Linux capabilities isolator is loaded when `linux/capabilities` is
present in the agent's `--isolation` flag.
Capabilities which should be allowed are passed with the
`--allowed_capabilities` flag. This isolator requires the
`CAP_SETPCAP` capability so agent processes typically need to be
started as root. A possible agent startup invocation could be

```{.console}
sudo mesos-agent --master=<master ip> --ip=<agent ip>
  --work_dir=/var/lib/mesos
  --isolation=linux/capabilities[,other isolation flags]
  --allowed_capabilities='{"capabilities":[NET_RAW,MKNOD]}'
```

An empty list for `--allowed_capabilities` signifies that no
capabilities are allowed, while an absent `--allowed_capabilities` flag
signifies that all capabilities are allowed.


## Task setup

In order for a Mesos task to acquire allowed capabilities it needs to
declare required capabilities in the `LinuxInfo` of its
`ContainerInfo`.

A Mesos task can only request capabilities which are allowed for the
agent; a task requesting unallowed capabilities will be rejected.

If an empty list of capabilities is given the Mesos task will drop all
capabilities; if the optional `capability_info` field is not set the
container will be able to acquire the capabilities of the Mesos task's
user.
