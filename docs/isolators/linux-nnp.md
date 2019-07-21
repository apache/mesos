---
title: Apache Mesos - Linux NNP (no\_new\_privs) Isolator in Mesos Containerizer
layout: documentation
---

# Linux NNP Support in Mesos Containerizer

This document describes the `linux/nnp` isolator. This isolator sets the
[no\_new\_privs](https://www.kernel.org/doc/Documentation/prctl/no_new_privs.txt)
flag for all containers launched using the [MesosContainerizer](../mesos-containerizer.md).

The `no_new_privs` flag disables the ability of container tasks to acquire any additional
privileges by means of executing a child process e.g. through invocation of `setuid` or
`setgid` programs. To enable the `linux/nnp` isolator, append `linux/nnp` to the
[`--isolation`](../configuration/agent.md#isolation) flag when starting the Mesos agent.
