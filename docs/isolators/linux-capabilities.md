# Linux Capabilities Support in Mesos Containerizer

This document describes the `linux/capabilities` isolator. The
isolator adds support for controlling [Linux
Capabilities](http://man7.org/linux/man-pages/man7/capabilities.7.html)
of containers launched using the
[MesosContainerizer](../mesos-containerizer.md)

The Linux capabilities isolator allows operators to control which
privileged operations Mesos tasks may perform. Operators can specify
which capabilities to allow for containers executing on an agent;
containers on the other hand can expose which capabilities they need.

See the protobuf definition of `CapabilityInfo::Capability` for the
list of currently exposed capabilities.


## Agent setup

The Linux capabilities isolator is loaded when `linux/capabilities` is
present in the agent's `--isolation` flag.  This isolator requires the
`CAP_SETPCAP` capability so agent processes typically need to be started
as root.

The `--effective_capabilities` flag specifies a set of capabilities that
are always granted to tasks. If the running kernel (Linux 4.3 or later)
supports ambient capabilities, these capabilities will be added to the
effective capability set of the task when it is launched. Otherwise
they must be re-acquired by arranging for the task to execute a file
with the relevant file-based capabilities enabled.

In the absence of capabilities specified by the scheduler, an empty list
for `--effective_capabilities` signifies that all capabilities will
be explicitly dropped.  If the `--effective_capabilities` flag is not
present, the task will be launched with the default capabilities of the
task user but the ambient capabilities will not be set.

The `--bounding_capabilities` flag specifies an upper bound on the
the capabilities a task is allowed to acquire or be granted.
Schedulers are not allowed to launch tasks with capabilities outside
the set specified by the `--bounding_capabilities` flag, but may
specify effective or bounding capabilities that are within this
set.

An empty list for `--bounding_capabilities` signifies that no capabilities
are allowed, while an absent `--bounding_capabilities` flag signifies
that all capabilities are allowed.

A possible agent startup invocation could be

```{.console}
sudo mesos-agent --master=<master ip> --ip=<agent ip>
  --work_dir=/var/lib/mesos
  --isolation=linux/capabilities[,other isolation flags]
  --effective_capabilities='{"capabilities":["NET_RAW","MKNOD"]}'
  --bounding_capabilities='{"capabilities":["NET_RAW","MKNOD","SYSLOG"]}'
```


## Task setup

In order for a Mesos task to acquire specific effective capabilities
or limit its bounding capabilities it should declare the required
capabilities in the `LinuxInfo` element of its `ContainerInfo`.

A Mesos task can only request capabilities which are allowed according
to the `--bounding_capabilities` flag of the agent; a task requesting
other capabilities will be rejected. When the `--bounding_capabilities`
flag is not present, all capability requests will be granted.

If the optional `effective_capabilities` field is not set, the value
of the `--effective_capabilities` flag will be used to populate the
task capabilities. If the optional `bounding_capabilities` field
is not set, the value of the `--bounding_capabilities` flag will
be used to populate the task capabilities. In both case, if an empty
list of capabilities is given, the Mesos task will drop all
capabilities in the corresponding set.
