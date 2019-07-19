---
title: Apache Mesos - IPC Namespace Isolator in Mesos Containerizer
layout: documentation
---

# IPC Namespace Isolator in Mesos Containerizer

The IPC Namespace isolator can be used on Linux to place container in a
distinct IPC namespace (for isolating System V IPC resources and POSIX
message queue) and provide the container its own /dev/shm (for isolating
POSIX shared memory). The benefits of this are:

1. Visibility: Any IPC objects created in the container are private and
   cannot be seen by any other containers.

2. Clean termination: When the container is destroyed, any IPC objects
   created in the container will be automatically removed.

To enable the IPC namespace isolator, append `namespaces/ipc` to the `--isolation`
flag when starting the agent. Note that `filesystem/linux` isolator is required
for turning on IPC namespace isolator.

Framework users can control the behavior of a container's IPC namespace
and /dev/shm by setting the `ContainerInfo.linux_info.ipc_mode` field:

1. If set to `SHARE_PARENT`, the container will share the IPC namespace and
   /dev/shm with its parent. If the container is a top level container,
   it will share the IPC namespace and /dev/shm with the agent host, if
   the container is a nested container, it will share the IPC namespace
   and /dev/shm with its parent container. The implication is if a nested
   container wants to share the IPC namespace and /dev/shm with the agent
   host, its parent container has to do it first.

2. If set to `PRIVATE`, the container will have its own IPC namespace and
   /dev/shm.

3. If not set, for backward compatibility we will keep the previous behavior:
   Top level container will have its own IPC namespace and nested container
   will share the IPC namespace with its parent container. If the container
   does not have its own rootfs, it will share agent's /dev/shm, otherwise
   it will have its own /dev/shm.

As a security measure, operators can disallow any containers to share the
agent's IPC namespace and /dev/shm by setting the agent flag
`--disallow_sharing_agent_ipc_namespace` to `true`. If this agent flag is set to `false`
and the framework requests to launch a top level container to share the
agent's IPC namespace and /dev/shm, the container launch will be rejected.

Framework users can specify the size of a container's /dev/shm in MB by
setting the `ContainerInfo.linux_info.shm_size` field, and operators can specify
the default size of /dev/shm via the agent flag `--default_container_shm_size`.
So if the `ContainerInfo.linux_info.shm_size` field is not set, the size of
container's /dev/shm will be value of the `--default_container_shm_size` agent
flag, if that flag is not set too, the size of the /dev/shm will be half
of the agent host RAM which is the default behavior of Linux. The
`ContainerInfo.linux_info.shm_size` field will be ignored for the container which
shares its parent's /dev/shm.

Please note that we only support setting the `ContainerInfo.linux_info.shm_size` field
when the `ContainerInfo.linux_info.ipc_mode` field is set to `PRIVATE`, otherwise the
container launch will be rejected.
