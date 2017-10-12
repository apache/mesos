---
title: Apache Mesos - Pid Namespace Isolator in Mesos Containerizer
layout: documentation
---

# Pid Namespace Isolator in Mesos Containerizer

The `namespaces/pid` isolator can be used to isolate each container in
a separate pid namespace with two main benefits:

1. Visibility: Processes running in the container (executor and
   descendants) are unable to see or signal processes outside the
   namespace.

2. Clean termination: Termination of the leading process in a pid
   namespace will result in the kernel terminating all other processes
   in the namespace.

You can turn on this isolator by specifying the `--isolation` agent
flag (i.e., `--isolation=namespaces/pid,...`). Note that
`filesystem/linux` isolator is required for turning on pid namespace
isolator.

The Launcher will use (2) during destruction of a container in
preference to the freezer cgroup, avoiding known kernel issues related
to freezing cgroups under OOM conditions.

`/proc` will be mounted for containers so tools such as `ps` will work
correctly.

To enable the PID Namespace isolator, append `namespaces/pid` to the
`--isolation` flag when starting the agent. By default, each container
will have its own PID namespace if this isolator is enabled.

Framework users can allow a container to share pid namespace with its
parent by setting the `ContainerInfo.linux_info.share_pid_namespace`
field to `true`. If the container is a top level container, it will
share the pid namespace with the agent. If the container is a nested
container, it will share the pid namespace with its parent container.
The container will have its own pid namespace if the
`ContainerInfo.linux_info.share_pid_namespace` field is set to `false`.

As a security measure, operators can disallow any container to share
the agent's PID namespace by setting the agent flag
`--disallow_sharing_agent_pid_namespace` to `true`. If this agent flag
is set as `true` and the framework requests to launch a top level
container which shares its pid namespace with the agent, the container
launch will be rejected.
