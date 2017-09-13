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

### The `namespaces/ipc` Isolator

The IPC Namespace isolator can be used on Linux to place tasks
in a distinct IPC namespace. The benefit of this is that any
[IPC objects](http://man7.org/linux/man-pages/man7/svipc.7.html) created
in the container will be automatically removed when the container is
destroyed.


### The `network/cni` Isolator

This is described in a [separate document](cni.md).


### The `linux/capabilities` Isolator

This is described in a [separate document](linux_capabilities.md).


### The `posix/rlimits` Isolator

This is described in a [separate document](posix_rlimits.md).
