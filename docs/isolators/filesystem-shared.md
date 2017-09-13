---
title: Apache Mesos - Shared Filesystem Isolator in Mesos Containerizer
layout: documentation
---

# Shared Filesystem Isolator in Mesos Containerizer

This document describes the `filesystem/shared` isolator. You can turn
on this isolator by specifying the `--isolation` agent flag (i.e.,
`--isolation=filesystem/shared,...`).

NOTE: This isolator has been deprecated in favor of using
`filesytem/linux` isolator.

The `filesystem/shared` isolator can optionally be used on Linux hosts
to enable modifications to each container's view of the shared
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
