---
layout: post
title: Mesos 0.18.0 Released
permalink: /blog/mesos-0-18-0-released/
published: true
post_author:
  display_name: Ian Downes
  twitter: ndwns
tags: Release, Containerizer
---

The latest Mesos release, [0.18.0](http://mesos.apache.org/downloads/) is now available for [download](http://mesos.apache.org/downloads). A major part of the 0.18.0 release is a refactor of the project’s isolation strategy, making it easier to develop and use different isolation technologies with Mesos including alternative implementations. The refactor does not introduce any new isolation features; instead, we’ve established a set of APIs that others can use to extend Mesos.

## Isolation -> Containerization

The first change is in terminology. The Mesos Slave now uses a Containerizer to provide an environment for each executor and its tasks to run in. Containerization includes resource isolation but is a more general concept that can encompass such things as packaging.

## Containerizer API

The Containerizer API specifies the interface between the slave and a Containerizer. An internal containerization implementation, called MesosContainerizer, includes all isolation features present in earlier releases of Mesos: basic isolation on Posix systems and CPU and memory isolation using Cgroups on Linux systems.

One of the goals of the refactor was to enable alternative containerizer implementations. In particular, we encourage the community to add containerizer implementations which _delegate_ containerization to other, existing technologies, e.g., LXC, Docker, VMware, Virtualbox, KVM and others.

The folks at [Mesosphere](http://mesosphere.io) are working on one approach for a general ExternalContainerizer which provides a simple shim interface between the Mesos slave and an external program that performs isolation. When complete, the ExternalContainerizer could connect to any external containerizer such as, for example, [Deimos](https://github.com/mesosphere/deimos), another Mesosphere project, to use Docker for containerization.

## Isolator API

A second goal of the refactor was to make it easier to both develop and use different isolation components of the MesosContainerizer. To do this, we separated the coordination logic from the isolation logic. Users of Mesos now have more granular control over which isolation components a slave uses.

MesosContainerizer accepts a list of Isolators, each implementing the Isolator API, to isolate resources. For example, the CgroupsMemIsolator uses the memory cgroup subsystem on Linux to limit the amount of memory available to an executor. We have plans to release new Isolators including a NetworkIsolator, a FilesystemIsolator, and a DiskIsolator and also to support isolation features provided by Linux namespaces such as pid and user namespaces.

![Mesos Containerizer Isolator APIs](/assets/img/documentation/containerizer_isolator_api.png)

## Cgroup layout change

While we were doing the refactor we took the opportunity to update the cgroup layout to current best practices, making it easier to run Mesos on recent Linux distributions and alongside other cgroup users such as LXC, Docker, and systemd. The expected layout has changed from a single mount point for all controllers to a separate mount point for each controller. This change requires reconfiguring the host system for every slave and we highly recommend a system reboot.

Further details on the upgrade procedure are in the 0.18.0 [upgrade document](http://mesos.apache.org/documentation/latest/upgrades) and, as always, please ask questions on the [mailing list](http://mesos.apache.org/community), file tickets via [JIRA](https://issues.apache.org/jira/browse/MESOS), or discuss on [IRC](http://mesos.apache.org/community).