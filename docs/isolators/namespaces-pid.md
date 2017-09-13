---
title: Apache Mesos - Pid Namespace Isolator in Mesos Containerizer
layout: documentation
---

# Pid Namespace Isolator in Mesos Containerizer

The `namespaces/pid` isolator can be used to isolate each container in
a separate pid namespace with two main benefits:

1. Visibility: Processes running in the container (executor and
   descendants) are unable to see or signal processes outside the
   namespace.

2. Clean termination: Termination of the leading process in a pid
   namespace will result in the kernel terminating all other processes
   in the namespace.

You can turn on this isolator by specifying the `--isolation` agent
flag (i.e., `--isolation=namespaces/pid,...`). Note that
`filesystem/linux` isolator is required for turning on pid namespace
isolator.

The Launcher will use (2) during destruction of a container in
preference to the freezer cgroup, avoiding known kernel issues related
to freezing cgroups under OOM conditions.

`/proc` will be mounted for containers so tools such as `ps` will work
correctly.
