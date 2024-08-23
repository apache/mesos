---
title: Apache Mesos - Cgroups v2 Support
layout: documentation
---

# Using Mesos on systems with Cgroups2 enabled

As part of the move towards Cgroups2, the Cgroups isolator has been updated to
support the updated interface, Changes are outlined below, and it is recommended
to read up on the [Cgroups v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)
documentation for an deeper understanding.

### Requirements

The `cgroups2` filesystem must be mounted at `/sys/fs/cgroup`. This allows Mesos
to pick the Cgroups2 Isolator when creating the Mesos Containerizer.

### Cgroup Names

A cgroup called “CGROUP_NAME” has a path `/sys/fs/cgroup/$CGROUP_NAME`. This
applies for all cgroups. A cgroup's name is the cgroup's path relative to
`/sys/fs/cgroup`, where the cgroup2 filesystem is mounted.

`flags.cgroups_root` (default: "mesos"): Root cgroup name.

The client has control over the name of the root cgroup subtree under
`/sys/fs/cgroup` that Mesos manages. The default name is “mesos”.

### Process Cgroup

Every process Mesos manages will have a cgroup, and a leaf cgroup under it which
contains the pids. This is done to adhere to the [No Internal Process Constraint](https://docs.kernel.org/admin-guide/cgroup-v2.html#no-internal-process-constraint)
imposed by Cgroups v2.

### Container

When the cgroups v2 isolator is `prepare`d for a new container, cgroups are
created for the new container. When the cgroups v2 isolator `isolate`s, the new
container is moved into it's leaf cgroup.

Container Non-leaf Cgroup: `<flags.cgroups_root>/<containerId>`

Container Leaf Cgroup: `<flags.cgroups_root>/<containerId>/leaf`

### Nested Containers

The Cgroups v2 isolator supports nested containers.

Unlike Cgroups v1, we now create cgroups for all containers, even if they
indicated they do not want their own resource isolation. This is to make it
easier to keep track of a container’s processes.

If a container does not wish to have its own resource isolation, it can pass in
a flag `share_cgroups` and the isolator will not update any controllers for it.

### Systemd Integration

We currently do not have systemd integration. This section should be updated
with our approach if systemd support is implemented.

### Linux Launcher & Cgroups v2 Isolator

On Linux systems that support cgroups v2, the Mesos Containerizer will use the [Linux Launcher](https://github.com/apache/mesos/blob/master/src/slave/containerizer/mesos/linux_launcher.cpp) and the [Cgroups v2 Isolator](https://github.com/apache/mesos/blob/master/src/slave/containerizer/mesos/isolators/cgroups2/cgroups2.cpp).

It’s recommended to review to code to gain a complete understanding of these steps.

Operations on startup:

- Linux Launcher `recover`: Parse the cgroups subtree rooted at
`flags.cgroups_root` to obtain container ids. Compares the persisted state to
the recovered dcontainers to determine what contains are orphans.
- Cgroups v2 Isolator `recover`: Create internal state to track recovered
containers. Calls `recover` on all of the controllers that are used by each of
the recovered containers.

Operations when a new container is started:

- Cgroups v2 Isolator `prepare`: Creates cgroups for the new container and adds
the container to isolator's internal state. Configures namespace creation flags
and mount setups; does not create mounts or namespaces. Calls `prepare` on all
of the controllers that are used by the new container.
- Linux Launcher `fork`: Forks the Mesos Agent process to create the new
container's process. Also moves the child processes into the container's leaf
cgroup. Creates mounts and namespaces.
- Cgroups v2 Isolator `watch`: Calls `watch` on each of the controllers that
are used by the container. When a resource-watch promise is resolved a handler
is invoked.
- Cgroups v2 Isolator `isolate`: Calls `isolate` on each of the controllers that
are used by the container. Then moves the container process into the container's
leaf cgroup; at this point the container is isolated.