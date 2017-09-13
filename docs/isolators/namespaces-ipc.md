---
title: Apache Mesos - IPC Namespace Isolator in Mesos Containerizer
layout: documentation
---

# IPC Namespace Isolator in Mesos Containerizer

The IPC Namespace isolator can be used on Linux to place tasks
in a distinct IPC namespace. The benefit of this is that any
[IPC objects](http://man7.org/linux/man-pages/man7/svipc.7.html) created
in the container will be automatically removed when the container is
destroyed.

To enable the IPC namespace isolator, append `namespaces/ipc` to the
`--isolation` flag when starting the agent.
