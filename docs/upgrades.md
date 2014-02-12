---
layout: documentation
---

# Upgrading Mesos
This document serves as a guide for users who wish to upgrade an existing mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

## Upgrading from 0.17.0 to 0.18.0.

In order to upgrade a running cluster:

Note: This upgrade requires a system reboot for slaves that use Linux cgroups for isolation.

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries then perform one of the following two steps, depending on if cgroups isolation is used:
  * [no cgroups]
      - Restart the slaves. The "--isolation" flag has changed and "process" has been deprecated in favor of "posix/cpu,posix/mem".
  * [cgroups]
      - Change from a single mountpoint for all controllers to separate mountpoints for each controller, e.g., /sys/fs/cgroup/memory/ and /sys/fs/cgroup/cpu/.
      - The suggested configuration is to mount a tmpfs filesystem to /sys/fs/cgroup and to let the slave mount the required controllers. However, the slave will also use previously mounted controllers if they are appropriately mounted under "--cgroups_hierarchy".
      - It has been observed that unmounting and remounting of cgroups from the single to separate configuration is unreliable and a reboot into the new configuration is strongly advised. Restart the slaves after reboot.
      - The "--cgroups_hierarchy" now defaults to "/sys/fs/cgroup". The "--cgroups_root" flag default remains "mesos".
      -  The "--isolation" flag has changed and "cgroups" has been deprecated in favor of "cgroups/cpu,cgroups/mem".
      - The "--cgroup_subsystems" flag is no longer required and will be ignored.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.16.0 to 0.17.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.15.0 to 0.16.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


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
