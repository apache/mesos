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

Note that the Posix Disk isolator `--container_disk_watch_interval`
does not apply to the XFS Disk isolator.
