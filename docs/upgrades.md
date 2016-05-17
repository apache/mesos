---
title: Apache Mesos - Upgrading Mesos
layout: documentation
---

# Upgrading Mesos

This document serves as a guide for users who wish to upgrade an existing Mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

## Overview

This section provides an overview of the changes for each version (in particular when upgrading from the next lower version). For more details please check the respective sections below.

We categorize the changes as follows:

    A New feature/behavior
    C Changed feature/behavior
    D Deprecated feature/behavior
    R Removed feature/behavior

<table class="table table-bordered" style="table-layout: fixed;">
  <thead>
    <tr>
      <th width="10%">
        Version
      </th>
      <th width="18%">
        Mesos Core
      </th>
      <th width="18%">
        Flags
      </th>
      <th width="18%">
        Framework API
      </th>
      <th width="18%">
        Module API
      </th>
      <th width="18%">
        Endpoints
      </th>
    </tr>
  </thead>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  0.29.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>CD <a href="#0-29-x-allocator-metrics">Allocator Metrics</a></li>
      <li>D <a href="#0-29-x-credentials">--credential(s) (plain text format)</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  0.28.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-28-x-resource-precision">Resource Precision</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-28-x-autherization-acls">Authentication ACLs</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  0.27.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>D <a href="#0-27-x-implicit-roles">--roles</a></li>
      <li>D <a href="#0-27-x-acl-shutdown-flag">--acls (shutdown_frameworks)</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-27-x-executor-lost-callback">executorLost callback</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-27-x-allocator-api">Allocator API</a></li>
      <li>C <a href="#0-27-x-isolator-api">Isolator API</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  0.26.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-26-x-taskstatus-reason">TaskStatus::Reason Enum</a></li>
      <li>C <a href="#0-26-x-credential-protobuf">Credential Protobuf</a></li>
      <li>C <a href="#0-26-x-network-info-protobuf">NetworkInfo Protobuf</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-26-x-state-endpoint">State Endpoint</a></li>
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  0.25.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#0-25-x-scheduler-bindings">C++/Java/Python Scheduler Bindings</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>D <a href="#0-25-x-json-endpoints">*.json Endpoints</a></li>
    </ul>
  </td>
</tr>
</table>

## Upgrading from 0.28.x to 0.29.x ##

<a name="0-29-x-allocator-metrics"></a>

* The allocator metric named <code>allocator/event_queue_dispatches</code> is now deprecated and will be removed with 0.30. The new name is <code>allocator/mesos/event_queue_dispatches</code> to better support metrics for alternative allocator implementations.

<a name="0-29-x-credentials"></a>
* Mesos 0.29 deprecates the use of plain text credential files in favor of JSON-formatted credential files.

* When a persistent volume is destroyed, Mesos will now remove any data that was stored on the volume from the filesystem of the appropriate agent. In prior versions of Mesos, destroying a volume would not delete data (this was a known missing feature that has now been implemented).

* Mesos 0.29 changes the HTTP status code of the following endpoints from `200 OK` to `202 Accepted`:
  * `/reserve`
  * `/unreserve`
  * `/create-volumes`
  * `/destroy-volumes`

## Upgrading from 0.27.x to 0.28.x ##

<a name="0-28-x-resource-precision"></a>

* Mesos 0.28 only supports three decimal digits of precision for scalar resource values. For example, frameworks can reserve "0.001" CPUs but more fine-grained reservations (e.g., "0.0001" CPUs) are no longer supported (although they did not work reliably in prior versions of Mesos anyway). Internally, resource math is now done using a fixed-point format that supports three decimal digits of precision, and then converted to/from floating point for input and output, respectively. Frameworks that do their own resource math and manipulate fractional resources may observe differences in roundoff error and numerical precision.

<a name="0-28-x-autherization-acls"></a>

* Mesos 0.28 changes the definitions of two ACLs used for authorization. The objects of the `ReserveResources` and `CreateVolume` ACLs have been changed to `roles`. In both cases, principals can now be authorized to perform these operations for particular roles. This means that by default, a framework or operator can reserve resources/create volumes for any role. To restrict this behavior, [ACLs can be added](authorization.md) to the master which authorize principals to reserve resources/create volumes for specified roles only. Previously, frameworks could only reserve resources for their own role; this behavior can be preserved by configuring the `ReserveResources` ACLs such that the framework's principal is only authorized to reserve for the framework's role. **NOTE** This renders existing `ReserveResources` and `CreateVolume` ACL definitions obsolete; if you are authorizing these operations, your ACL definitions should be updated.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

## Upgrading from 0.26.x to 0.27.x ##

<a name="0-27-x-implicit-roles"></a>

* Mesos 0.27 introduces the concept of _implicit roles_. In previous releases, configuring roles required specifying a static whitelist of valid role names on master startup (via the `--roles` flag). In Mesos 0.27, if `--roles` is omitted, _any_ role name can be used; controlling which principals are allowed to register as which roles should be done using [ACLs](authorization.md). The role whitelist functionality is still supported but is deprecated.

<a name="0-27-x-allocator-api"></a>

* The Allocator API has changed due to the introduction of implicit roles. Custom allocator implementations will need to be updated. See [MESOS-4000](https://issues.apache.org/jira/browse/MESOS-4000) for more information.

<a name="0-27-x-executor-lost-callback"></a>

* The `executorLost` callback in the Scheduler interface will now be called whenever the agent detects termination of a custom executor. This callback was never called in previous versions, so please make sure any framework schedulers can now safely handle this callback. Note that this callback may not be reliably delivered.

<a name="0-27-x-isolator-api"></a>

* The isolator `prepare` interface has been changed slightly. Instead of keeping adding parameters to the `prepare` interface, we decide to use a protobuf (`ContainerConfig`). Also, we renamed `ContainerPrepareInfo` to `ContainerLaunchInfo` to better capture the purpose of this struct. See [MESOS-4240](https://issues.apache.org/jira/browse/MESOS-4240) and [MESOS-4282](https://issues.apache.org/jira/browse/MESOS-4282) for more information. If you are an isolator module writer, you will have to adjust your isolator module according to the new interface and re-compile with 0.27.

<a name="0-27-x-acl-shutdown-flag"></a>

* ACLs.shutdown_frameworks has been deprecated in favor of the new ACLs.teardown_frameworks. This affects the `--acls` master flag for the local authorizer.

* Reserved resources are now accounted for in the DRF role sorter. Previously unaccounted reservations will influence the weighted DRF sorter. If role weights were explicitly set, they may need to be adjusted in order to account for the reserved resources in the cluster.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

## Upgrading from 0.25.x to 0.26.x ##

<a name="0-26-x-taskstatus-reason"></a>

* The names of some TaskStatus::Reason enums have been changed. But the tag numbers remain unchanged, so it is backwards compatible. Frameworks using the new version might need to do some compile time adjustments:

  * REASON_MEM_LIMIT -> REASON_CONTAINER_LIMITATION_MEMORY
  * REASON_EXECUTOR_PREEMPTED -> REASON_CONTAINER_PREEMPTED

<a name="0-26-x-credential-protobuf"></a>

* The `Credential` protobuf has been changed. `Credential` field `secret` is now a string, it used to be bytes. This will affect framework developers and language bindings ought to update their generated protobuf with the new version. This fixes JSON based credentials file support.

<a name="0-26-x-state-endpoint"></a>

* The `/state` endpoints on master and agent will no longer include `data` fields as part of the JSON models for `ExecutorInfo` and `TaskInfo` out of consideration for memory scalability (see [MESOS-3794](https://issues.apache.org/jira/browse/MESOS-3794) and [this email thread](http://www.mail-archive.com/dev@mesos.apache.org/msg33536.html)).
  * On master, the affected `data` field was originally found via `frameworks[*].executors[*].data`.
  * On agents, the affected `data` field was originally found via `executors[*].tasks[*].data`.

<a name="0-26-x-network-info-protobuf"></a>

* The `NetworkInfo` protobuf has been changed. The fields `protocol` and `ip_address` are now deprecated. The new field `ip_addresses` subsumes the information provided by them.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

## Upgrading from 0.24.x to 0.25.x

<a name="0-25-x-json-endpoints"></a>

* The following endpoints will be deprecated in favor of new endpoints. Both versions will be available in 0.25 but the deprecated endpoints will be removed in a subsequent release.

  For master endpoints:

  * /state.json becomes /state
  * /tasks.json becomes /tasks

  For agent endpoints:

  * /state.json becomes /stateπ
  * /monitor/statistics.json becomes /monitor/statisticsπ

  For both master and agent:

  * /files/browse.json becomes /files/browse
  * /files/debug.json becomes /files/debug
  * /files/download.json becomes /files/download
  * /files/read.json becomes /files/read

<a name="0-25-x-scheduler-bindings"></a>

* The C++/Java/Python scheduler bindings have been updated. In particular, the driver can make a suppressOffers() call to stop receiving offers (until reviveOffers() is called).

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).


## Upgrading from 0.23.x to 0.24.x

* Support for live upgrading a driver based scheduler to HTTP based (experimental) scheduler has been added.

* Master now publishes its information in ZooKeeper in JSON (instead of protobuf). Make sure schedulers are linked against >= 0.23.0 libmesos before upgrading the master.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).


## Upgrading from 0.22.x to 0.23.x

* The 'stats.json' endpoints for masters and agents have been removed. Please use the 'metrics/snapshot' endpoints instead.

* The '/master/shutdown' endpoint is deprecated in favor of the new '/master/teardown' endpoint.

* In order to enable decorator modules to remove metadata (environment variables or labels), we changed the meaning of the return value for decorator hooks in Mesos 0.23.0. Please refer to the modules documentation for more details.

* Agent ping timeouts are now configurable on the master via `--slave_ping_timeout` and `--max_slave_ping_timeouts`. Agents should be upgraded to 0.23.x before changing these flags.

* A new scheduler driver API, `acceptOffers`, has been introduced. This is a more general version of the `launchTasks` API, which allows the scheduler to accept an offer and specify a list of operations (Offer.Operation) to perform using the resources in the offer. Currently, the supported operations include LAUNCH (launching tasks), RESERVE (making dynamic reservations), UNRESERVE (releasing dynamic reservations), CREATE (creating persistent volumes) and DESTROY (releasing persistent volumes). Similar to the `launchTasks` API, any unused resources will be considered declined, and the specified filters will be applied on all unused resources.

* The Resource protobuf has been extended to include more metadata for supporting persistence (DiskInfo), dynamic reservations (ReservationInfo) and oversubscription (RevocableInfo). You must not combine two Resource objects if they have different metadata.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).


## Upgrading from 0.21.x to 0.22.x

* Agent checkpoint flag has been removed as it will be enabled for all
agents. Frameworks must still enable checkpointing during registration to take advantage
of checkpointing their tasks.

* The stats.json endpoints for masters and agents have been deprecated.
Please refer to the metrics/snapshot endpoint.

* The C++/Java/Python scheduler bindings have been updated. In particular, the driver can be constructed with an additional argument that specifies whether to use implicit driver acknowledgements. In `statusUpdate`, the `TaskStatus` now includes a UUID to make explicit acknowledgements possible.

* The Authentication API has changed slightly in this release to support additional authentication mechanisms. The change from 'string' to 'bytes' for AuthenticationStartMessage.data has no impact on C++ or the over-the-wire representation, so it only impacts pure language bindings for languages like Java and Python that use different types for UTF-8 strings vs. byte arrays.

    message AuthenticationStartMessage {
      required string mechanism = 1;
      optional bytes data = 2;
    }


* All Mesos arguments can now be passed using file:// to read them out of a file (either an absolute or relative path). The --credentials, --whitelist, and any flags that expect JSON backed arguments (such as --modules) behave as before, although support for just passing an absolute path for any JSON flags rather than file:// has been deprecated and will produce a warning (and the absolute path behavior will be removed in a future release).

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Install the new agent binaries and restart the agents.
3. Upgrade the schedulers:
  * For Java schedulers, link the new native library against the new JAR. The JAR contains API above changes. A 0.21.0 JAR will work with a 0.22.0 libmesos. A 0.22.0 JAR will work with a 0.21.0 libmesos if explicit acks are not being used. 0.22.0 and 0.21.0 are inter-operable at the protocol level between the master and the scheduler.
  * For Python schedulers, upgrade to use a 0.22.0 egg. If constructing `MesosSchedulerDriverImpl` with `Credentials`, your code must be updated to pass the `implicitAcknowledgements` argument before `Credentials`. You may run a 0.21.0 Python scheduler against a 0.22.0 master, and vice versa.
4. Restart the schedulers.
5. Upgrade the executors by linking the latest native library / jar / egg.


## Upgrading from 0.20.x to 0.21.x

* Disabling agent checkpointing has been deprecated; the agent --checkpoint flag has been deprecated and will be removed in a future release.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Install the new agent binaries and restart the agents.
3. Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
4. Restart the schedulers.
5. Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.19.x to 0.20.x.

* The Mesos API has been changed slightly in this release. The CommandInfo has been changed (see below), which makes launching a command more flexible. The 'value' field has been changed from _required_ to _optional_. However, it will not cause any issue during the upgrade (since the existing schedulers always set this field).

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

* The Python bindings are also changing in this release. There are now sub-modules which allow you to use either the interfaces and/or the native driver.

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

* If you're using a pure language binding, please ensure that it sends status update acknowledgements through the master before upgrading.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Install the new agent binaries and restart the agents.
3. Upgrade the schedulers by linking the latest native library (install the latest mesos jar and python egg if necessary).
4. Restart the schedulers.
5. Upgrade the executors by linking the latest native library (install the latest mesos jar and python egg if necessary).

## Upgrading from 0.18.x to 0.19.x.

* There are new required flags on the master (`--work_dir` and `--quorum`) to support the *Registrar* feature, which adds replicated state on the masters.

* No required upgrade ordering across components.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Install the new agent binaries and restart the agents.
3. Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
4. Restart the schedulers.
5. Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.17.0 to 0.18.x.

* This upgrade requires a system reboot for agents that use Linux cgroups for isolation.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
3. Restart the schedulers.
4. Install the new agent binaries then perform one of the following two steps, depending on if cgroups isolation is used:
  * [no cgroups]
    - Restart the agents. The "--isolation" flag has changed and "process" has been deprecated in favor of "posix/cpu,posix/mem".
  * [cgroups]
    - Change from a single mountpoint for all controllers to separate mountpoints for each controller, e.g., /sys/fs/cgroup/memory/ and /sys/fs/cgroup/cpu/.
    - The suggested configuration is to mount a tmpfs filesystem to /sys/fs/cgroup and to let the agent mount the required controllers. However, the agent will also use previously mounted controllers if they are appropriately mounted under "--cgroups_hierarchy".
    - It has been observed that unmounting and remounting of cgroups from the single to separate configuration is unreliable and a reboot into the new configuration is strongly advised. Restart the agents after reboot.
    - The "--cgroups_hierarchy" now defaults to "/sys/fs/cgroup". The "--cgroups_root" flag default remains "mesos".
    -  The "--isolation" flag has changed and "cgroups" has been deprecated in favor of "cgroups/cpu,cgroups/mem".
    - The "--cgroup_subsystems" flag is no longer required and will be ignored.
5. Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.16.0 to 0.17.0.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
3. Restart the schedulers.
4. Install the new agent binaries and restart the agents.
5. Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.15.0 to 0.16.0.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
3. Restart the schedulers.
4. Install the new agent binaries and restart the agents.
5. Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.14.0 to 0.15.0.

* Schedulers should implement the new `reconcileTasks` driver method.
* Schedulers should call the new `MesosSchedulerDriver` constructor that takes `Credential` to authenticate.
* --authentication=false (default) allows both authenticated and unauthenticated frameworks to register.

In order to upgrade a running cluster:

1. Install the new master binaries.
2. Restart the masters with --credentials pointing to credentials of the framework(s).
3. Install the new agent binaries and restart the agents.
4. Upgrade the executors by linking the latest native library and mesos jar (if necessary).
5. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
6. Restart the schedulers.
 Restart the masters with --authentication=true.

NOTE: After the restart unauthenticated frameworks *will not* be allowed to register.


## Upgrading from 0.13.0 to 0.14.0.

* /vars endpoint has been removed.

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Upgrade the executors by linking the latest native library and mesos jar (if necessary).
3. Install the new agent binaries.
4. Restart the agents after adding --checkpoint flag to enable checkpointing.
5. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
6. Set FrameworkInfo.checkpoint in the scheduler if checkpointing is desired (recommended).
7. Restart the schedulers.
8. Restart the masters (to get rid of the cached FrameworkInfo).
9. Restart the agents (to get rid of the cached FrameworkInfo).

## Upgrading from 0.12.0 to 0.13.0.

* cgroups_hierarchy_root agent flag is renamed as cgroups_hierarchy

In order to upgrade a running cluster:

1. Install the new master binaries and restart the masters.
2. Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
3. Restart the schedulers.
4. Install the new agent binaries.
5. Restart the agents.
6. Upgrade the executors by linking the latest native library and mesos jar (if necessary).

## Upgrading from 0.11.0 to 0.12.0.

* If you are a framework developer, you will want to examine the new 'source' field in the ExecutorInfo protobuf. This will allow you to take further advantage of the resource monitoring.

In order to upgrade a running cluster:

1. Install the new agent binaries and restart the agents.
2. Install the new master binaries and restart the masters.
