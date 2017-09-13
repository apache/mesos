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

## Isolators

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
