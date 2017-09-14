---
title: Apache Mesos - Mesos Containerizer
layout: documentation
---

# Mesos Containerizer

The Mesos Containerizer provides lightweight containerization and
resource isolation of executors using Linux-specific functionality
such as control cgroups and namespaces. It is composable so operators
can selectively enable different [isolators](#isolators).

It also provides basic support for POSIX systems (e.g., OSX) but
without any actual isolation, only resource usage reporting.

## Isolators

Isolators are components that each define an aspect of how a tasks
execution environment (or container) is constructed. Isolators can
control how containers are isolated from each other, how task resource
limits are enforced, how networking is configured, how security
policies are applied.

Since the isolator interface is [modularized](modules.md), operators
can write modules that implement custom isolators.

Mesos supports the following built-in isolators (not a complete list).

- [cgroups/net_cls](isolators/cgroups-net-cls.md)
- [disk/du](isolators/disk-du.md)
- [disk/xfs](isolators/disk-xfs.md)
- [docker/runtime](isolators/docker-runtime.md)
- [docker/volume](isolators/docker-volume.md)
- [filesystem/shared](isolators/filesystem-shared.md)
- [gpu/nvidia](gpu-support.md)
- [linux/capabilities](isolators/linux-capabilities.md)
- [namespaces/ipc](isolators/namespaces-ipc.md)
- [namespaces/pid](isolators/namespaces-pid.md)
- [network/cni](cni.md)
- [network/port_mapping](isolators/network-port-mapping.md)
- [posix/rlimits](isolators/posix-rlimits.md)
- [volume/host_path](container-volume.md#host_path-volume-source)
- [volume/sandbox_path](container-volume.md#sandbox_path-volume-source)
