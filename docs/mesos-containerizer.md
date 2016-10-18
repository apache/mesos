---
title: Apache Mesos - Mesos Containerizer
layout: documentation
---

# Mesos Containerizer

The MesosContainerizer provides lightweight containerization and
resource isolation of executors using Linux-specific functionality
such as control cgroups and namespaces. It is composable so operators
can selectively enable different isolators.

It also provides basic support for POSIX systems (e.g., OSX) but
without any actual isolation, only resource usage reporting.


### Shared Filesystem

The SharedFilesystem isolator can optionally be used on Linux hosts to
enable modifications to each container's view of the shared
filesystem.

The modifications are specified in the ContainerInfo included in the
ExecutorInfo, either by a framework or by using the
`--default_container_info` agent flag.

ContainerInfo specifies Volumes which map parts of the shared
filesystem (host\_path) into the container's view of the filesystem
(container\_path), as read-write or read-only. The host\_path can be
absolute, in which case it will make the filesystem subtree rooted at
host\_path also accessible under container\_path for each container.
If host\_path is relative then it is considered as a directory
relative to the executor's work directory. The directory will be
created and permissions copied from the corresponding directory (which
must exist) in the shared filesystem.

The primary use-case for this isolator is to selectively make parts of
the shared filesystem private to each container. For example, a
private "/tmp" directory can be achieved with `host_path="tmp"` and
`container_path="/tmp"` which will create a directory "tmp" inside the
executor's work directory (mode 1777) and simultaneously mount it as
/tmp inside the container. This is transparent to processes running
inside the container. Containers will not be able to see the host's
/tmp or any other container's /tmp.


### Pid Namespace

The Pid Namespace isolator can be used to isolate each container in
a separate pid namespace with two main benefits:

1. Visibility: Processes running in the container (executor and
   descendants) are unable to see or signal processes outside the
   namespace.

2. Clean termination: Termination of the leading process in a pid
   namespace will result in the kernel terminating all other processes
   in the namespace.

The Launcher will use (2) during destruction of a container in
preference to the freezer cgroup, avoiding known kernel issues related
to freezing cgroups under OOM conditions.

/proc will be mounted for containers so tools such as 'ps' will work
correctly.


### Posix Disk Isolator

The Posix Disk isolator provides basic disk isolation. It is able to
report the disk usage for each sandbox and optionally enforce the disk
quota. It can be used on both Linux and OS X.

To enable the Posix Disk isolator, append `disk/du` to the `--isolation`
flag when starting the agent.

By default, the disk quota enforcement is disabled. To enable it,
specify `--enforce_container_disk_quota` when starting the agent.

The Posix Disk isolator reports disk usage for each sandbox by
periodically running the `du` command. The disk usage can be retrieved
from the resource statistics endpoint ([/monitor/statistics](endpoints/slave/monitor/statistics.md)).

The interval between two `du`s can be controlled by the agent flag
`--container_disk_watch_interval`. For example,
`--container_disk_watch_interval=1mins` sets the interval to be 1
minute. The default interval is 15 seconds.


### XFS Disk Isolator

The XFS Disk isolator uses XFS project quotas to track the disk
space used by each container sandbox and to enforce the corresponding
disk space allocation. Write operations performed by tasks exceeding
their disk allocation will fail with an `EDQUOT` error. The task
will not be terminated by the containerizer.

The XFS disk isolator is functionally similar to Posix Disk isolator
but avoids the cost of repeatedly running the `du`.  Though they will
not interfere with each other, it is not recommended to use them together.

To enable the XFS Disk isolator, append `disk/xfs` to the `--isolation`
flag when starting the agent.

The XFS Disk isolator requires the sandbox directory to be located
on an XFS filesystem that is mounted with the `pquota` option. There
is no need to configure
[projects](http://man7.org/linux/man-pages/man5/projects.5.html)
or [projid](http://man7.org/linux/man-pages/man5/projid.5.html)
files. The range of project IDs given to the `--xfs_project_range`
must not overlap any project IDs allocated for other uses.

The XFS disk isolator does not natively support an accounting-only mode
like that of the Posix Disk isolator. Quota enforcement can be disabled
by mounting the filesystem with the `pqnoenforce` mount option.

The [xfs_quota](http://man7.org/linux/man-pages/man8/xfs_quota.8.html)
command can be used to show the current allocation of project IDs
and quota. For example:

    $ xfs_quota -x -c "report -a -n -L 5000 -U 1000"

To show which project a file belongs to, use the
[xfs_io](http://man7.org/linux/man-pages/man8/xfs_io.8.html) command
to display the `fsxattr.projid` field. For example:

    $ xfs_io -r -c stat /mnt/mesos/

Note that the Posix Disk isolator flags `--enforce_container_disk_quota`,
`--container_disk_watch_interval` and `--enforce_container_disk_quota` do
not apply to the XFS Disk isolator.


### Docker Runtime Isolator

The Docker Runtime isolator is used for supporting runtime
configurations from the docker image (e.g., Entrypoint/Cmd, Env,
etc.). This isolator is tied with `--image_providers=docker`. If
`--image_providers` contains `docker`, this isolator must be used.
Otherwise, the agent will refuse to start.

To enable the Docker Runtime isolator, append `docker/runtime` to the
`--isolation` flag when starting the agent.

Currently, docker image default `Entrypoint`, `Cmd`, `Env`, and `WorkingDir` are
supported with docker runtime isolator. Users can specify `CommandInfo` to
override the default `Entrypoint` and `Cmd` in the image (see below for
details). The `CommandInfo` should be inside of either `TaskInfo` or
`ExecutorInfo` (depending on whether the task is a command task or uses a custom
executor, respectively).

#### Determine the Launch Command

If the user specifies a command in `CommandInfo`, that will override the
default Entrypoint/Cmd in the docker image. Otherwise, we will use the
default Entrypoint/Cmd and append arguments specified in `CommandInfo`
accordingly. The details are explained in the following table.

Users can specify `CommandInfo` including `shell`, `value` and
`arguments`, which are represented in the first column of the table
below. `0` represents `not specified`, while `1` represents
`specified`. The first row is how `Entrypoint` and `Cmd` defined in
the docker image. All cells in the table, except the first column and
row, as well as cells labeled as `Error`, have the first element
(i.e., `/Entrypt[0]`) as executable, and the rest as appending
arguments.

<table class="table table-striped">
  <tr>
    <th></th>
    <th>Entrypoint=0<br>Cmd=0</th>
    <th>Entrypoint=0<br>Cmd=1</th>
    <th>Entrypoint=1<br>Cmd=0</th>
    <th>Entrypoint=1<br>Cmd=1</th>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>/Cmd[0]<br>Cmd[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>Cmd..</td>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>/Cmd[0]<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=0</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=1</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=0</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=1</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
</table>


### The `cgroups/net_cls` Isolator

The cgroups/net_cls isolator allows operators to provide network
performance isolation and network segmentation for containers within
a Mesos cluster. To enable the cgroups/net_cls isolator, append
`cgroups/net_cls` to the `--isolation` flag when starting the agent.

As the name suggests, the isolator enables the net_cls subsystem for
Linux cgroups and assigns a net_cls cgroup to each container launched
by the `MesosContainerizer`.  The objective of the net_cls subsystem
is to allow the kernel to tag packets originating from a container
with a 32-bit handle. These handles can be used by kernel modules such
as `qdisc` (for traffic engineering) and `net-filter` (for
firewall) to enforce network performance and security policies
specified by the operators.  The policies, based on the net_cls
handles, can be specified by the operators through user-space tools
such as
[tc](http://tldp.org/HOWTO/Traffic-Control-HOWTO/software.html#s-iproute2-tc)
and [iptables](http://linux.die.net/man/8/iptables).

The 32-bit handle associated with a net_cls cgroup can be specified by
writing the handle to the `net_cls.classid` file, present within the
net_cls cgroup. The 32-bit handle is of the form `0xAAAABBBB`, and
consists of a 16-bit primary handle 0xAAAA and a 16-bit secondary
handle 0xBBBB. You can read more about the use cases for the primary
and secondary handles in the [Linux kernel documentation for
net_cls](https://www.kernel.org/doc/Documentation/cgroup-v1/net_cls.txt).

By default, the cgroups/net_cls isolator does not manage the net_cls
handles, and assumes the operator is going to manage/assign these
handles. To enable the management of net_cls handles by the
cgroups/net_cls isolator you need to specify a 16-bit primary handle,
of the form 0xAAAA, using the `--cgroups_net_cls_primary_handle` flag at
agent startup.

Once a primary handle has been specified for an agent, for each
container the cgroups/net_cls isolator allocates a 16-bit secondary
handle. It then assigns the 32-bit combination of the primary and
secondary handle to the net_cls cgroup associated with the container
by writing to `net_cls.classid`. The cgroups/net_cls isolator exposes
the assigned net_cls handle to operators by exposing the handle as
part of the `ContainerStatus` &mdash;associated with any task running within
the container&mdash; in the agent's [/state](endpoints/slave/state.md) endpoint.


### The `docker/volume` Isolator

This is described in a [separate document](docker-volume.md).


### The `network/cni` Isolator

This is described in a [separate document](cni.md).

### The `linux/capabilities` Isolator

This is described in a [separate document](linux_capabilities.md).
