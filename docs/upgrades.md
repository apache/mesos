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
  1.2.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-2-x-heartbeat-flag">http_heartbeat_interval</a></li>
      <li>A <a href="#1-2-x-backend-flag">image_provisioner_backend</a></li>
      <li>A <a href="#1-2-x-unreachable-flag">max_unreachable_tasks_per_framework</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-2-x-revive-suppress">Revive and Suppress v1 scheduler Calls</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-2-x-container-logger-interface">Container Logger prepare method</a></li>
      <li>C <a href="#1-2-x-allocator-module-changes">Allocator module changes</a></li>
      <li>A <a href="#1-2-x-new-authz-actions">New Authorizer module actions</a></li>
      <li>D <a href="#1-2-x-renamed-authz-actions">Renamed Authorizer module actions (deprecated old aliases)</a></li>
      <li>R <a href="#1-2-x-removed-hooks">Removed slavePreLaunchDockerEnvironmentDecorator and slavePreLaunchDockerHook</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-2-x-debug-endpoints">LAUNCH_NESTED_CONTAINER_SESSION, ATTACH_CONTAINER_INPUT, ATTACH_CONTAINER_OUTPUT</a></li>
      <li>D <a href="#1-2-x-recovered-frameworks">v1 GetFrameworks recovered_frameworks</a></li>
      <li>D <a href="#1-2-x-orphan-executors">v1 GetExecutors orphan_executors</a></li>
      <li>D <a href="#1-2-x-orphan-tasks">v1 GetTasks orphan_tasks</a></li>
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.1.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>R <a href="#1-1-x-container-logger-interface">Container Logger recovery method</a></li>
      <li>C <a href="#1-1-x-allocator-updateallocation">Allocator updateAllocation method</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.0.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>CD <a href="#1-0-x-allocator-metrics">Allocator Metrics</a></li>
      <li>C <a href="#1-0-x-persistent-volume">Destruction of persistent volumes</a></li>
      <li>C <a href="#1-0-x-slave">Slave to Agent rename</a></li>
      <li>C <a href="#1-0-x-quota-acls">Quota ACLs</a></li>
      <li>R <a href="#1-0-x-executor-environment-variables">Executor environment variables inheritance</a></li>
      <li>R <a href="#1-0-x-deprecated-fields-in-container-config">Deprecated fields in ContainerConfig</a></li>
      <li>C <a href="#1-0-x-persistent-volume-ownership">Persistent volume ownership</a></li>
      <li>C <a href="#1-0-x-fetcher-user">Fetcher assumes same user as task</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>D <a href="#1-0-x-docker-timeout-flag">docker_stop_timeout</a></li>
      <li>D <a href="#1-0-x-credentials-file">credential(s) (plain text format)</a></li>
      <li>C <a href="#1-0-x-slave">Slave to Agent rename</a></li>
      <li>R <a href="#1-0-x-workdir">work_dir default value</a></li>
      <li>D <a href="#1-0-x-deprecated-ssl-env-variables">SSL environment variables</a></li>
      <li>ACD <a href="#1-0-x-http-authentication-flags">HTTP authentication</a></li>
      <li>R <a href="#1-0-x-registry-strict">registry_strict</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>DC <a href="#1-0-x-executorinfo">ExecutorInfo.source</a></li>
      <li>A <a href="#1-0-x-v1-commandinfo">CommandInfo.URI output_file</a></li>
      <li>C <a href="#1-0-x-scheduler-proto">scheduler.proto optional fields</a></li>
      <li>C <a href="#1-0-x-executor-proto">executor.proto optional fields</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-0-x-authorizer">Authorizer</a></li>
      <li>C <a href="#1-0-x-allocator">Allocator</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-0-x-status-code">HTTP return codes</a></li>
      <li>R <a href="#1-0-x-status-code">/observe</a></li>
      <li>C <a href="#1-0-x-endpoint-authorization">Added authorization</a></li>
    </ul>
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

## Upgrading from 1.1.x to 1.2.x ##

<a name="1-2-x-heartbeat-flag"></a>
* New Agent flag http_heartbeat_interval: This flag sets a heartbeat interval for messages to be sent over persistent connections made against the agent HTTP API. Currently, this only applies to the LAUNCH_NESTED_CONTAINER_SESSION and ATTACH_CONTAINER_OUTPUT calls. (default: 30secs)

<a name="1-2-x-backend-flag"></a>
* New Agent flag image_provisioner_backend: Strategy for provisioning container rootfs from images, e.g., aufs, bind, copy, overlay.

<a name="1-2-x-unreachable-flag"></a>
* New Master flag max_unreachable_tasks_per_framework: Maximum number of unreachable tasks per framework to store in memory. (default: 1000)

<a name="1-2-x-revive-suppress"></a>
* New Revive and Suppress v1 scheduler Calls: Revive or Suppress offers for a specified role. If role is unset, the call will revive/suppress offers for all of the roles the framework is subscribed to. (Especially for multi-role frameworks.)

<a name="1-2-x-container-logger-interface"></a>
* Mesos 1.2 modifies the `ContainerLogger`'s `prepare()` method.  The method now takes an additional argument for the `user` the logger should run a subprocess as.  Please see [MESOS-5856](https://issues.apache.org/jira/browse/MESOS-5856) for more information.

<a name="1-2-x-allocator-module-changes"></a>
* Allocator module changes to support inactive frameworks, multi-role frameworks, and suppress/revive. See `allocator.hpp` for interface changes.

<a name="1-2-x-new-authz-actions"></a>
* New Authorizer module actions: LAUNCH_NESTED_CONTAINER, KILL_NESTED_CONTAINER, WAIT_NESTED_CONTAINER, LAUNCH_NESTED_CONTAINER_SESSION, ATTACH_CONTAINER_INPUT, ATTACH_CONTAINER_OUTPUT, VIEW_CONTAINER, and SET_LOG_LEVEL. See `authorizer.proto` for module interface changes, and `acls.proto` for corresponding LocalAuthorizer ACL changes.

<a name="1-2-x-renamed-authz-actions"></a>
* Renamed Authorizer module actions (and deprecated old aliases): REGISTER_FRAMEWORK, TEARDOWN_FRAMEWORK, RESERVE_RESOURCES, UNRESERVE_RESOURCES, CREATE_VOLUME, DESTROY_VOLUME, UPDATE_WEIGHT, GET_QUOTA. See `authorizer.proto` for interface changes.

<a name="1-2-x-removed-hooks"></a>
* Removed slavePreLaunchDockerEnvironmentDecorator and slavePreLaunchDockerHook in favor of slavePreLaunchDockerTaskExecutorDecorator.

<a name="1-2-x-debug-endpoints"></a>
* New Agent v1 operator API calls: LAUNCH_NESTED_CONTAINER_SESSION, ATTACH_CONTAINER_INPUT, ATTACH_CONTAINER_OUTPUT for debugging into running containers (Mesos containerizer only).

<a name="1-2-x-recovered-frameworks"></a>
* Deprecated `recovered_frameworks` in v1 GetFrameworks call. Now it will be empty.

<a name="1-2-x-orphan-executors"></a>
* Deprecated `orphan_executors` in v1 GetExecutors call. Now it will be empty.

<a name="1-2-x-orphan-tasks"></a>
* Deprecated `orphan_tasks` in v1 GetTasks call. Now it will be empty.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents/schedulers can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

## Upgrading from 1.0.x to 1.1.x ##

<a name="1-1-x-container-logger-interface"></a>

* Mesos 1.1 removes the `ContainerLogger`'s `recover()` method.  The `ContainerLogger` had an incomplete interface for a stateful implementation.  This removes the incomplete parts to avoid adding tech debt in the containerizer.  Please see [MESOS-6371](https://issues.apache.org/jira/browse/MESOS-6371) for more information.

<a name="1-1-x-allocator-updateallocation"></a>

* Mesos 1.1 adds an `offeredResources` argument to the `Allocator::updateAllocation()` method. It is used to indicate the resources that the operations passed to `updateAllocation()` are applied to. [MESOS-4431](https://issues.apache.org/jira/browse/MESOS-4431) (paticularly [/r/45961/](https://reviews.apache.org/r/45961/)) has more details on the motivation.

## Upgrading from 0.28.x to 1.0.x ##

<a name="1-0-x-deprecated-ssl-env-variables"></a>

* Prior to Mesos 1.0, environment variables prefixed by `SSL_` are used to control libprocess SSL support. However, it was found that those environment variables may collide with some libraries or programs (e.g., openssl, curl). From Mesos 1.0, `SSL_*` environment variables are deprecated in favor of the corresponding `LIBPROCESS_SSL_*` variables.

<a name="1-0-x-persistent-volume-ownership"></a>

* Prior to Mesos 1.0, Mesos agent recursively changes the ownership of the persistent volumes every time they are mounted to a container. From Mesos 1.0, this behavior has been changed. Mesos agent will do a _non-recursive_ change of ownership of the persistent volumes.

<a name="1-0-x-deprecated-fields-in-container-config"></a>

* Mesos 1.0 removed the camel cased protobuf fields in `ContainerConfig` (see `include/mesos/slave/isolator.proto`):
  * `required ExecutorInfo executorInfo = 1;`
  * `optional TaskInfo taskInfo = 2;`

<a name="1-0-x-executor-environment-variables"></a>

* By default, executors will no longer inherit environment variables from the agent. The operator can still use the `--executor-environment-variables` flag on the agent to explicitly specify what environment variables the executors will get. Mesos generated environment variables (i.e., `$MESOS_`, `$LIBPROCESS_`) will not be affected. If `$PATH` is not specified for an executor, a default value `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin` will be used.

<a name="1-0-x-allocator-metrics"></a>

* The allocator metric named <code>allocator/event_queue_dispatches</code> is now deprecated. The new name is <code>allocator/mesos/event_queue_dispatches</code> to better support metrics for alternative allocator implementations.

<a name="1-0-x-docker-timeout-flag"></a>

* The `--docker_stop_timeout` agent flag is deprecated.

<a name="1-0-x-executorinfo"></a>

* The ExecutorInfo.source field is deprecated in favor of ExecutorInfo.labels.

<a name="1-0-x-slave"></a>

* Mesos 1.0 deprecates the 'slave' keyword in favor of 'agent' in a number of places
  * Deprecated flags with keyword 'slave' in favor of 'agent'.
  * Deprecated sandbox links with 'slave' keyword in the WebUI.
  * Deprecated `slave` subcommand for mesos-cli.

<a name="1-0-x-workdir"></a>

* Mesos 1.0 removes the default value for the agent's `work_dir` command-line flag. This flag is now required; the agent will exit immediately if it is not provided.

<a name="1-0-x-registry-strict"></a>

* Mesos 1.0 disables support for the master's `registry_strict` command-line flag. If this flag is specified, the master will exit immediately. Note that this flag was previously marked as experimental and not recommended for production use.

<a name="1-0-x-credentials-file"></a>

* Mesos 1.0 deprecates the use of plain text credential files in favor of JSON-formatted credential files.

<a name="1-0-x-persistent-volume"></a>

* When a persistent volume is destroyed, Mesos will now remove any data that was stored on the volume from the filesystem of the appropriate agent. In prior versions of Mesos, destroying a volume would not delete data (this was a known missing feature that has now been implemented).

<a name="1-0-x-status-code"></a>

* Mesos 1.0 changes the HTTP status code of the following endpoints from `200 OK` to `202 Accepted`:
  * `/reserve`
  * `/unreserve`
  * `/create-volumes`
  * `/destroy-volumes`

<a name="1-0-x-v1-commandinfo"></a>

* Added `output_file` field to CommandInfo.URI in Scheduler API and v1 Scheduler HTTP API.

<a name="1-0-x-scheduler-proto"></a>

* Changed Call and Event Type enums in scheduler.proto from required to optional for the purpose of backwards compatibility.

<a name="1-0-x-executor-proto"></a>

* Changed Call and Event Type enums in executor.proto from required to optional for the purpose of backwards compatibility.

<a name="1-0-x-nonterminal"></a>

* Added non-terminal task metadata to the container resource usage information.

<a name="1-0-x-observe-endpoint"></a>

* Deleted the /observe HTTP endpoint.

<a name="1-0-x-quota-acls"></a>

* The `SetQuota` and `RemoveQuota` ACLs have been deprecated. To replace these, a new ACL `UpdateQuota` have been introduced. In addition, a new ACL `GetQuota` have been added; these control which principals are allowed to query quota information for which roles. These changes affect the `--acls` flag for the local authorizer in the following ways:
  * The `update_quotas` ACL cannot be used in combination with either the `set_quotas` or `remove_quotas` ACL. The local authorizor will produce an error in such a case;
  * When upgrading a Mesos cluster that uses the `set_quotas` or `remove_quotas` ACLs, the operator should first upgrade the Mesos binaries. At this point, the deprecated ACLs will still be enforced. After the upgrade has been verified, the operator should replace deprecated values for `set_quotas` and `remove_quotas` with equivalent values for `update_quotas`;
  * If desired, the operator can use the `get_quotas` ACL after the upgrade to control which principals are allowed to query quota information.

<a name="1-0-x-authorizer"></a>

* Mesos 1.0 contains a number of authorizer changes that particularly effect custom authorizer modules:
  * The authorizer interface has been refactored in order to decouple the ACL definition language from the interface. It additionally includes the option of retrieving `ObjectApprover`. An `ObjectApprover` can be used to synchronously check authorizations for a given object and is hence useful when authorizing a large number of objects and/or large objects (which need to be copied using request-based authorization). NOTE: This is a **breaking change** for authorizer modules.
  * Authorization-based HTTP endpoint filtering enables operators to restrict which parts of the cluster state a user is authorized to see. Consider for example the `/state` master endpoint: an operator can now authorize users to only see a subset of the running frameworks, tasks, or executors.
  * The `subject` and `object` fields in the authorization::Request protobuf message have been changed to be optional. If these fields are not set, the request should only be allowed for ACLs with `ANY` semantics. NOTE: This is a semantic change for authorizer modules.

<a name="1-0-x-allocator"></a>

* Namespace and header file of `Allocator` has been moved to be consistent with other packages.

<a name="1-0-x-fetcher-user"></a>

* When a task is run as a particular user, the fetcher now fetches files as that user also. Note, this means that filesystem permissions for that user will be enforced when fetching local files.

<a name="1-0-x-http-authentication-flags"></a>

* The `--authenticate_http` flag has been deprecated in favor of `--authenticate_http_readwrite`. Setting `--authenticate_http_readwrite` will now enable authentication for all endpoints which previously had authentication support. These happen to be the endpoints which allow modifiication of the cluster state, or "read-write" endpoints. Note that `/logging/toggle`, `/profiler/start`, `/profiler/stop`, `/maintenance/schedule`, `/machine/up`, and `/machine/down` previously did not have authentication support, but in 1.0 if either `--authenticate_http` or `--authenticate_http_readwrite` is set, those endpoints will now require authentication. A new flag has also been introduced, `--authenticate_http_readonly`, which enables authentication for endpoints which support authentication and do not allow modification of the state of the cluster, like `/state` or `/flags`.

<a name="1-0-x-endpoint-authorization"></a>

* Mesos 1.0 introduces authorization support for several HTTP endpoints. Note that some of these endpoints are used by the web UI, and thus using the web UI in a cluster with authorization enabled will require that ACLs be set appropriately. Please refer to the [authorization documentation](authorization.md) for details.

* The endpoints with coarse-grained authorization enabled are:
  - `/files/debug`
  - `/logging/toggle`
  - `/metrics/snapshot`
  - `/slave(id)/containers`
  - `/slave(id)/monitor/statistics`

* If the defined ACLs used `permissive: false`, the listed HTTP endpoints will stop working unless ACLs for the `get_endpoints` actions are defined.

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

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
