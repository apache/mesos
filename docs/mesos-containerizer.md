---
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
--default\_container\_info slave flag.

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
private "/tmp" directory can be achieved with host\_path="tmp" and
container\_path="/tmp" which will create a directory "tmp" inside the
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
