---
layout: documentation
---

# Upgrading Mesos

This document serves as a guide for users who wish to upgrade an existing mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

## Upgrading from 0.23.x to 0.24.2 ##

* Mesos 0.24.2 only supports three decimal digits of precision for scalar resource values. For example, frameworks can reserve "0.001" CPUs but more fine-grained reservations (e.g., "0.0001" CPUs) are no longer supported (although they did not work reliably in prior versions of Mesos anyway). Internally, resource math is now done using a fixed-point format that supports three decimal digits of precision, and then converted to/from floating point for input and output, respectively. Frameworks that do their own resource math and manipulate fractional resources may observe differences in roundoff error and numerical precision.

## Upgrading from 0.23.x to 0.24.x

**NOTE** Support for live upgrading a driver based scheduler to HTTP based (experimental) scheduler has been added.

**NOTE** Master now publishes its information in ZooKeeper in JSON (instead of protobuf). Make sure schedulers are linked against >= 0.23.0 libmesos before upgrading the master.

In order to upgrade a running cluster:

* Rebuild and install any modules so that upgraded masters/slaves can use them.
* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library / jar / egg (if necessary).


## Upgrading from 0.22.x to 0.23.x

**NOTE** The 'stats.json' endpoints for masters and slaves have been removed. Please use the 'metrics/snapshot' endpoints instead.

**NOTE** The '/master/shutdown' endpoint is deprecated in favor of the new '/master/teardown' endpoint.

**NOTE** In order to enable decorator modules to remove metadata (environment variables or labels), we changed the meaning of the return value for decorator hooks in Mesos 0.23.0. Please refer to the modules documentation for more details.

**NOTE** Slave ping timeouts are now configurable on the master via `--slave_ping_timeout` and `--max_slave_ping_timeouts`. Slaves should be upgraded to 0.23.x before changing these flags.

**NOTE** A new scheduler driver API, `acceptOffers`, has been introduced. This is a more general version of the `launchTasks` API, which allows the scheduler to accept an offer and specify a list of operations (Offer.Operation) to perform using the resources in the offer. Currently, the supported operations include LAUNCH (launching tasks), RESERVE (making dynamic reservations), UNRESERVE (releasing dynamic reservations), CREATE (creating persistent volumes) and DESTROY (releasing persistent volumes). Similar to the `launchTasks` API, any unused resources will be considered declined, and the specified filters will be applied on all unused resources.

**NOTE** The Resource protobuf has been extended to include more metadata for supporting persistence (DiskInfo), dynamic reservations (ReservationInfo) and oversubscription (RevocableInfo). You must not combine two Resource objects if they have different metadata.

In order to upgrade a running cluster:

* Rebuild and install any modules so that upgraded masters/slaves can use them.
* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library / jar / egg (if necessary).


## Upgrading from 0.21.x to 0.22.x

**NOTE** Slave checkpoint flag has been removed as it will be enabled for all
slaves. Frameworks must still enable checkpointing during registration to take advantage
of checkpointing their tasks.

**NOTE** The stats.json endpoints for masters and slaves have been deprecated.
Please refer to the metrics/snapshot endpoint.

**NOTE** The C++/Java/Python scheduler bindings have been updated. In particular, the driver can be constructed with an additional argument that specifies whether to use implicit driver acknowledgements. In `statusUpdate`, the `TaskStatus` now includes a UUID to make explicit acknowledgements possible.

**NOTE**: The Authentication API has changed slightly in this release to support additional authentication mechanisms. The change from 'string' to 'bytes' for AuthenticationStartMessage.data has no impact on C++ or the over-the-wire representation, so it only impacts pure language bindings for languages like Java and Python that use different types for UTF-8 strings vs. byte arrays.

    message AuthenticationStartMessage {
      required string mechanism = 1;
      optional bytes data = 2;
    }


**NOTE** All Mesos arguments can now be passed using file:// to read them out of a file (either an absolute or relative path). The --credentials, --whitelist, and any flags that expect JSON backed arguments (such as --modules) behave as before, although support for just passing a absolute path for any JSON flags rather than file:// has been deprecated and will produce a warning (and the absolute path behavior will be removed in a future release).

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers:
  * For Java schedulers, link the new native library against the new JAR. The JAR contains API changes per the **NOTE** above. A 0.21.0 JAR will work with a 0.22.0 libmesos. A 0.22.0 JAR will work with a 0.21.0 libmesos if explicit acks are not being used. 0.22.0 and 0.21.0 are inter-operable at the protocol level between the master and the scheduler.
  * For Python schedulers, upgrade to use a 0.22.0 egg. If constructing `MesosSchedulerDriverImpl` with `Credentials`, your code must be updated to pass the `implicitAcknowledgements` argument before `Credentials`. You may run a 0.21.0 Python scheduler against a 0.22.0 master, and vice versa.
* Restart the schedulers.
* Upgrade the executors by linking the latest native library / jar / egg.


## Upgrading from 0.20.x to 0.21.x

**NOTE** Disabling slave checkpointing has been deprecated; the slave --checkpoint flag has been deprecated and will be removed in a future release.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.19.x to 0.20.x.

**NOTE**: The Mesos API has been changed slightly in this release. The CommandInfo has been changed (see below), which makes launching a command more flexible. The 'value' field has been changed from _required_ to _optional_. However, it will not cause any issue during the upgrade (since the existing schedulers always set this field).

    message CommandInfo {
      ...
      // There are two ways to specify the command:
      // 1) If 'shell == true', the command will be launched via shell
      //    (i.e., /bin/sh -c 'value'). The 'value' specified will be
      //    treated as the shell command. The 'arguments' will be ignored.
      // 2) If 'shell == false', the command will be launched by passing
      //    arguments to an executable. The 'value' specified will be
      //    treated as the filename of the executable. The 'arguments'
      //    will be treated as the arguments to the executable. This is
      //    similar to how POSIX exec families launch processes (i.e.,
      //    execlp(value, arguments(0), arguments(1), ...)).
      optional bool shell = 6 [default = true];
      optional string value = 3;
      repeated string arguments = 7;
      ...
    }

**NOTE**: The Python bindings are also changing in this release. There are now sub-modules which allow you to use either the interfaces and/or the native driver.

* `import mesos.native` for the native drivers
* `import mesos.interface` for the stub implementations and protobufs

To ensure a smooth upgrade, we recommend to upgrade your python framework and executor first. You will be able to either import using the new configuration or the old. Replace the existing imports with something like the following:

    try:
        from mesos.native import MesosExecutorDriver, MesosSchedulerDriver
        from mesos.interface import Executor, Scheduler
        from mesos.interface import mesos_pb2
    except ImportError:
        from mesos import Executor, MesosExecutorDriver, MesosSchedulerDriver, Scheduler
        import mesos_pb2

**NOTE**: If you're using a pure language binding, please ensure that it sends status update acknowledgements through the master before upgrading.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (install the latest mesos jar and python egg if necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library (install the latest mesos jar and python egg if necessary).

## Upgrading from 0.18.x to 0.19.x.

**NOTE**: There are new required flags on the master (`--work_dir` and `--quorum`) to support the *Registrar* feature, which adds replicated state on the masters.

**NOTE**: No required upgrade ordering across components.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.17.0 to 0.18.x.

In order to upgrade a running cluster:

**NOTE**: This upgrade requires a system reboot for slaves that use Linux cgroups for isolation.

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
