---
title: Apache Mesos - XFS Disk Isolator in Mesos Containerizer
layout: documentation
---

# XFS Disk Isolator in Mesos Containerizer

The `disk/xfs` isolator uses XFS project quotas to track the disk space
used by each container sandbox and to enforce the corresponding disk
space allocation. When quota enforcement is enabled, write operations
performed by tasks exceeding their disk allocation will fail with an
`EDQUOT` error. The task will not be terminated by the containerizer.

To enable the XFS Disk isolator, append `disk/xfs` to the `--isolation`
flag when starting the agent.

The XFS Disk isolator supports the `--enforce_container_disk_quota` flag.
If enforcement is enabled, the isolator will set both the hard and soft
quota limit. Otherwise, no limits will be set, Disk usage accounting
will be performed but the task will be allowed to exceed its allocation.

The XFS Disk isolator requires the sandbox directory to be located
on an XFS filesystem that is mounted with the `pquota` option. There
is no need to configure
[projects](http://man7.org/linux/man-pages/man5/projects.5.html)
or [projid](http://man7.org/linux/man-pages/man5/projid.5.html)
files. The range of project IDs given to the `--xfs_project_range`
must not overlap any project IDs allocated for other uses.

The [xfs_quota](http://man7.org/linux/man-pages/man8/xfs_quota.8.html)
command can be used to show the current allocation of project IDs
and quota. For example:

    $ xfs_quota -x -c "report -a -n -L 5000 -U 10000"

To show which project a file belongs to, use the
[xfs_io](http://man7.org/linux/man-pages/man8/xfs_io.8.html) command
to display the `fsxattr.projid` field. For example:

    $ xfs_io -r -c stat /mnt/mesos/

Project IDs are not reclaimed until the sandboxes they were assigned to
are garbage collected. The XFS Disk isolator periodically checks if
sandboxes of terminated containers still exist and deallocates project
IDs of the ones that were removed. Such checks are performed at
intervals specified by the
[`--disk_watch_interval`](configuration/agent.md#disk_watch_interval)
flag. Current number of available project IDs and total number of
project IDs used by the isolator can be tracked using
`containerizer/mesos/disk/project_ids_free` and
`containerizer/mesos/disk/project_ids_total` metrics.

## Killing containers

The XFS Disk isolator flag `--xfs_kill_containers` will create container
quotas that have a gap between the soft and hard limits. The soft limit is
equal to the limit requested for the `disk` resource and the hard limit
is 10MB higher. If a container violates the soft limit then it will be
killed. The isolator polls for soft limit violations at the interval
specified by the `--container_disk_watch_interval` flag.

Note that the `--container_disk_watch_interval` flag only applies to
the XFS Disk isolator when `--xfs_kill_containers` is set to true.
