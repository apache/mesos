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

Mesos supports the following built-in isolators.

- appc/runtime
- [cgroups/blkio](isolators/cgroups-blkio.md)
- [cgroups/cpu](isolators/cgroups-cpu.md)
- cgroups/cpuset
- [cgroups/devices](isolators/cgroups-devices.md)
- cgroups/hugetlb
- cgroups/mem
- [cgroups/net_cls](isolators/cgroups-net-cls.md)
- cgroups/net\_prio
- cgroups/perf\_event
- cgroups/pids
- [disk/du](isolators/disk-du.md)
- [disk/xfs](isolators/disk-xfs.md)
- [docker/runtime](isolators/docker-runtime.md)
- [docker/volume](isolators/docker-volume.md)
- [environment\_secret](secrets.md#environment-based-secrets)
- [filesystem/linux](isolators/filesystems.md)
- [filesystem/posix](isolators/filesystems.md)
- [filesystem/shared](isolators/filesystem-shared.md)
- filesystem/windows
- [gpu/nvidia](gpu-support.md)
- [linux/capabilities](isolators/linux-capabilities.md)
- [linux/devices](isolators/linux-devices.md)
- [linux/nnp](isolators/linux-nnp.md)
- [linux/seccomp](isolators/linux-seccomp.md)
- [namespaces/ipc](isolators/namespaces-ipc.md)
- [namespaces/pid](isolators/namespaces-pid.md)
- [network/cni](cni.md)
- [network/port_mapping](isolators/network-port-mapping.md)
- [network/ports](isolators/network-ports.md)
- posix/cpu
- posix/mem
- [posix/rlimits](isolators/posix-rlimits.md)
- [volume/csi](isolators/csi-volume.md)
- [volume/host_path](container-volume.md#host_path-volume-source)
- volume/image
- [volume/sandbox_path](container-volume.md#sandbox_path-volume-source)
- [volume/secret](secrets.md#file-based-secrets)
- [windows/cpu](isolators/windows.md#cpu-limits)
- [windows/mem](isolators/windows.md#memory-limits)

## Systemd Integration

To prevent systemd from manipulating cgroups managed by the agent,
it's recommended to add 'Delegate' under 'Service' in the service
unit file of Mesos agent, for example:

```
[Service]
Delegate=true
```
