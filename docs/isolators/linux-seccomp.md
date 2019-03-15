---
title: Apache Mesos - Linux Seccomp Isolator in Mesos Containerizer
layout: documentation
---

# Linux Seccomp Support in Mesos Containerizer

This document describes the `linux/seccomp` isolator. This isolator adds support
for installing [Seccomp filter](http://man7.org/linux/man-pages/man2/seccomp.2.html)
for containers launched using the [MesosContainerizer](../mesos-containerizer.md).

Seccomp filter reduces the attack surface of the Linux kernel by providing a
mechanism for filtering of certain system calls. Seccomp requires Linux kernel
3.5 or newer.

Seccomp filter is defined by a Seccomp profile which must be compatible with
the Docker Seccomp profile format. An example of the Seccomp profile can be
found in [default.json](../../examples/seccomp_default.json).

**Note**: Mesos containerizer uses `pivot_root` system call, so it **must be**
specified in the Seccomp profile. Usually, the Docker Seccomp profile contains
`chroot` syscall, so the `pivot_root` syscall must be added to the same array
`"names": ["chroot","pivot_root"]`.

## Agent setup

The Linux Seccomp isolator is loaded when `linux/seccomp` is present in the
agent's `--isolation` flag. This isolator requires root privileges to install
a Seccomp filter because a Seccomp filter can't be installed for a
non-privileged user without setting `no_new_privs` bit which leads to side
effects.

The `--seccomp_config_dir` flag specifies the path to the directory containing
Seccomp profiles.

The `--seccomp_profile_name` flag specifies the default Seccomp profile which is
applied by default for all Mesos containers. This profile name must be relative
to the `--seccomp_config_dir`. If this flag is omitted, then the default Seccomp
profile is not defined and therefore not applied by default.

A possible agent startup invocation could be

```{.console}
sudo mesos-agent --master=<master ip> --ip=<agent ip>
  --work_dir=/var/lib/mesos
  --isolation=linux/seccomp[,other isolation flags]
  --seccomp_config_dir=/etc/mesos/seccomp
  --seccomp_profile_name=default.json
```


## Task setup

In order for a Mesos task to override the agent's default Seccomp profile,
it should declare the required profile in the `LinuxInfo` field of its
`ContainerInfo`. E.g., if the agent is launched with the default Seccomp
profile enabled, a framework can disable Seccomp for a particular task by
setting an `unconfined` field in the corresponding `SeccompInfo`.
