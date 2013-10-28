---
layout: page
---

# Upgrading Mesos
This document serves as a guide for users who wish to upgrade an existing mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

## Upgrading from 0.14.0 to 0.15.0.

In order to upgrade a running cluster:

* Install the new master binaries.
* Restart the masters with --credentials pointing to credentials of the framework(s).
* NOTE: --authentication=false (default) allows both authenticated and unauthenticated frameworks to register.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* NOTE: Schedulers should implement the new `reconcileTasks` driver method.
* Schedulers should call the new `MesosSchedulerDriver` constructor that takes `Credential` to authenticate.
* Restart the schedulers.
* Restart the masters with --authentication=true.
* NOTE: After the restart unauthenticated frameworks *will not* be allowed to register.


## Upgrading from 0.13.0 to 0.14.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* NOTE: /vars endpoint has been removed.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).
* Install the new slave binaries.
* Restart the slaves after adding --checkpoint flag to enable checkpointing.
* NOTE: /vars endpoint has been removed.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Set FrameworkInfo.checkpoint in the scheduler if checkpointing is desired (recommended).
* Restart the schedulers.
* Restart the masters (to get rid of the cached FrameworkInfo).
* Restart the slaves (to get rid of the cached FrameworkInfo).

## Upgrading from 0.12.0 to 0.13.0.
In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries.
* NOTE: cgroups_hierarchy_root slave flag is renamed as cgroups_hierarchy
* Restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).

## Upgrading from 0.11.0 to 0.12.0.
In order to upgrade a running cluster:

* Install the new slave binaries and restart the slaves.
* Install the new master binaries and restart the masters.

If you are a framework developer, you will want to examine the new 'source' field in the ExecutorInfo protobuf. This will allow you to take further advantage of the resource monitoring.
