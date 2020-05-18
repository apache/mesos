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
  1.10.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>D <a href="#1-10-x-ssl-env-var-rename">Renamed LIBPROCESS_SSL_VERIFY_CERT and LIBPROCESS_SSL_REQUIRE_CERT environment variables.</a></li>
      <li>D <a href="#1-10-x-limits-cfs-quota">CPU limits affect the function of the agent's `cgroups_enable_cfs` flag.</a></li>
    </ul>
 </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-10-x-agent-features">agent_features</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-10-x-synchronous-authorization">Authorizers must support synchronous authorization.</a></li>
      <li>AC <a href="#1-10-x-allocator-module-changes">Resource consumption is exposed to allocators.</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>D <a href="#1-10-x-tasks-pending-authoirization-deprecated">v1 GetTasks pending_tasks</a></li>
    </ul>
  </td>
</tr>

<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.9.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-9-x-quota-guarantees">Quota Limits</a></li>
      <li>A <a href="#1-9-x-linux-nnp-isolator">Linux NNP isolator</a></li>
      <li>A <a href="#1-9-x-hostname-validation-scheme">hostname_validation_scheme</a></li>
      <li>C <a href="#1-9-x-client-certificate-verification">TLS certificate verification behaviour</a></li>
      <li>C <a href="#1-9-x-configurable-ipc">Configurable IPC namespace and /dev/shm</a></li>
      <li>A <a href="#1-9-x-automatic-agent-draining">Automatic Agent Draining</a></li>
    </ul>
 </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-9-x-docker-ignore-runtime">docker_ignore_runtime</a></li>
      <li>A <a href="#1-9-x-configurable-ipc">disallow_sharing_agent_ipc_namespace</a></li>
      <li>A <a href="#1-9-x-configurable-ipc">default_container_shm_size</a></li>
      <li>C <a href="#1-9-x-agent-features">agent_features</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-9-x-configurable-ipc">LinuxInfo.ipc_mode and LinuxInfo.shm_size</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>D <a href="#1-9-x-update-quota">SET_QUOTA and REMOVE QUOTA deprecated
            in favor of UPDATE_QUOTA</a></li>
      <li>D <a href="#1-9-x-quota-guarantees">Quota guarantees deprecated in favor
            of using quota limits</a></li>
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.8.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-8-x-linux-seccomp-isolator">Linux Seccomp isolator</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-8-x-linux-seccomp-isolator">seccomp_config_dir</a></li>
      <li>A <a href="#1-8-x-linux-seccomp-isolator">seccomp_profile_name</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.7.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-7-x-linux-devices-isolator">Linux devices isolator</a></li>
      <li>A <a href="#1-7-x-auto-load-subsystems">Automatically load local enabled cgroups subsystems</a></li>
      <li>A <a href="#1-7-x-container-specific-cgroups-mounts">Container-specific cgroups mounts</a></li>
      <li>A <a href="#1-7-x-volume-mode-support">Volume mode support</a></li>
      <li>A <a href="#1-7-x-resource-provider-acls">Resource Provider ACLs</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-7-x-enforce-container-ports">enforce_container_ports</a></li>
      <li>A <a href="#1-7-x-gc-non-executor-container-sandboxes">gc_non_executor_container_sandboxes</a></li>
      <li>A <a href="#1-7-x-network-cni-root-dir-persist">network_cni_root_dir_persist</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-7-x-create-disk">`CREATE_DISK` and `DESTROY_DISK` operations and ACLs</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-7-x-container-logger">ContainerLogger module interface changes</a></li>
      <li>C <a href="#1-7-x-isolator-recover">Isolator::recover module interface changes</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-7-x-json-serialization">JSON serialization changes</a></li>
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.6.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-6-x-grpc-requirement">Requirement for gRPC library</a></li>
      <li>C <a href="#1-6-x-csi-support">CSI v0.2 Support</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-6-x-fetcher-stall-timeout">fetcher_stall_timeout</a></li>
    </ul>
    <ul style="padding-left:10px;">
      <li>A <a href="#1-6-x-xfs-kill-containers">xfs_kill_containers</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-6-x-disk-profile-adaptor">Disk profile adaptor module changes</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.5.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-5-x-task-starting">Built-in executors send a TASK_STARTING update</a></li>
      <li>A <a href="#1-5-x-network-ports-isolator">Network ports isolator</a></li>
      <li>C <a href="#1-5-x-relative-disk-source-root-path">Relative source root paths for disk resources</a></li>
      <li>A <a href="#1-5-x-reconfiguration-policy">Agent state recovery after resource changes</a></li>
      <li>C <a href="#1-5-x-protobuf-requirement">Requirement for Protobuf library</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-5-x-network-ports-isolator">container_ports_watch_interval</a></li>
      <li>A <a href="#1-5-x-network-ports-isolator">check_agent_port_range_only</a></li>
      <li>D <a href="#1-5-x-executor-secret-key">executor_secret_key</a></li>
      <li>A <a href="#1-5-x-reconfiguration-policy">reconfiguration_policy</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-5-x-task-resource-limitation">Added the TaskStatus.limitation message</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-5-x-get-containers">Allowed to view nested/standalone containers</a></li>
    </ul>
  </td>

</tr>

<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.4.x
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-4-x-ambient-capabilities">Container capabilities are made ambient if supported</a></li>
      <li>A <a href="#1-4-x-bounding-capabilities">Support for explicit bounding capabilities</a></li>
      <li>C <a href="#1-4-x-agent-recovery">Agent recovery post reboot</a></li>
      <li>C <a href="#1-4-x-xfs-no-enforce">XFS disk isolator support for not enforcing disk limits</a></li>
      <li>C <a href="#1-4-x-update-minimal-docker-version">Update the minimal supported Docker version</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-4-x-agent-capabilities-flags">effective_capabilities</a></li>
      <li>A <a href="#1-4-x-agent-capabilities-flags">bounding-capabilities</a></li>
      <li>D <a href="#1-4-x-agent-capabilities-flags">allowed-capabilities</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-4-x-bounding-capabilities">Support for explicit setting bounding capabilities</a></li>
      <li>D <a href="#1-4-x-linuxinfo-capabilities">LinuxInfo.effective_capabilities deprecates LinuxInfo.capabilities</a></li>
      <li>C <a href="#1-4-x-mesos-library">`Resources` class in the internal Mesos C++ library only supports post-`RESERVATION_REFINEMENT` format</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-4-x-allocator-update-slave">Changed semantics of Allocator::updateSlave</a></li>
    </ul>
  </td>

  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
    </ul>
  </td>

</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.3.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>R <a href="#1-3-x-disallow-old-agents">Prevent registration by old Mesos agents</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Flags-->
    <ul style="padding-left:10px;">
      <li>R <a href="#1-3-x-setquota-removequota-acl">--acls (set_quotas and remove_quotas)</a></li>
      <li>R <a href="#1-3-x-shutdown-framework-acl">--acls (shutdown_frameworks)</a></li>
      <li>A <a href="#1-3-x-executor-authentication">authenticate_http_executors</a></li>
      <li>A <a href="#1-3-x-executor-authentication">executor_secret_key</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Framework API-->
    <ul style="padding-left:10px;">
      <li>A <a href="#1-3-x-multi-role-support">MULTI_ROLE support</a></li>
      <li>D <a href="#1-3-x-framework-info-role">FrameworkInfo.roles deprecates FrameworkInfo.role</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Module API-->
    <ul style="padding-left:10px;">
      <li>C <a href="#1-3-x-allocator-interface-change">Allocator MULTI_ROLE interface changes</a></li>
    </ul>
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Endpoints-->
    <ul style="padding-left:10px;">
      <li>D <a href="#1-3-x-endpoints-roles">MULTI_ROLE deprecates 'role' field in endpoints</a></li>
    </ul>
  </td>
</tr>
<tr>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Version-->
  1.2.x
  </td>
  <td style="word-wrap: break-word; overflow-wrap: break-word;"><!--Mesos Core-->
    <ul style="padding-left:10px;">
      <li>R <a href="#1-2-1-disallow-old-agents">Prevent registration by old Mesos agents</a></li>
    </ul>
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

## Upgrading from 1.9.x to 1.10.x ##

<a name="1-10-x-ssl-env-var-rename"></a>

* The canonical name for the environment variable `LIBPROCESS_SSL_VERIFY_CERT` was changed to `LIBPROCESS_SSL_VERIFY_SERVER_CERT`.
  The canonical name for the environment variable `LIBPROCESS_SSL_REQUIRE_CERT` was changed to `LIBPROCESS_SSL_REQUIRE_CLIENT_CERT`.
  The old names will continue to work as before, but operators are encouraged to update their configuration to reduce confusion.

<a name="1-10-x-limits-cfs-quota"></a>

* The Mesos agent's `cgroups_enable_cfs` flag previously controlled whether or not CFS quota would be used for all tasks on the agent. Resource limits have been added to tasks, and when a CPU limit is specified on a task, the agent will now apply a CFS quota regardless of the value of `cgroups_enable_cfs`.

<a name="1-10-x-agent-features"></a>

* The Mesos agent now requires the new `TASK_RESOURCE_LIMITS` feature. This capability is set by default, but if the `--agent_features` flag is specified explicitly, `TASK_RESOURCE_LIMITS` must be included.

<a name="1-10-x-synchronous-authorization"></a>

* Authorizers now must implement a method `getApprover(...)` (see the
  [authorization documentation](authorization.md#implementing-an-authorizer)
  and [MESOS-10056](https://issues.apache.org/jira/browse/MESOS-10056))
  that returns `ObjectApprover`s that are valid throughout their whole lifetime.
  Keeping the state of an `ObjectApprover` up-to-date becomes a responsibility
  of the authorizer. This is a **breaking change** for authorizer modules.

<a name="1-10-x-tasks-pending-authoirization-deprecated"></a>

* The field `pending_tasks` in `GetTasks` master API call has been deprecated.
  From now on, this field will be empty. Moreover, the notion of
  *tasks pending authorization* no longer exists
  (see [MESOS-10056](https://issues.apache.org/jira/browse/MESOS-10056)).


<a name="1-10-x-allocator-module-changes"></a>

* Allocator interface has been changed to supply allocator with information on
  resources actually consumed by frameworks. A method
  `transitionOfferedToAllocated(...)` has been added and the signature of
  `recoverResources(...)` has been extended. Note that allocators must implement
  these new/extended method signatures, but are free to ignore resource
  consumption data provided by master.

## Upgrading from 1.8.x to 1.9.x ##

<a name="1-9-x-automatic-agent-draining"></a>

* A new `DRAINING` state has been added to Mesos agents. Once an agent is draining, all tasks running on that agent are gracefully
  killed and no offers for that agent are sent to schedulers, preventing the launching of new tasks.
  Operators can put an agent into `DRAINING` state by using the `DRAIN_AGENT` operator API call.
  See [`docs/maintenance`](maintenance.md) for details.

<a name="1-9-x-agent-features"></a>

* The Mesos agent now requires the new `AGENT_DRAINING` feature. This capability is set by default, but if the `--agent_features` flag is specified explicitly, `AGENT_DRAINING` must be included.

<a name="1-9-x-linux-nnp-isolator"></a>

* A new [`linux/nnp`](isolators/linux-nnp.md) isolator has been added. The isolator supports setting of the `no_new_privs` bit in the container, preventing tasks from acquiring additional privileges.

<a name="1-9-x-docker-ignore-runtime"></a>

* A new [`--docker_ignore_runtime`](configuration/agent.md#docker_ignore_runtime) flag has been added. This causes the agent to ignore any runtime configuration present in Docker images.

<a name="1-9-x-hostname-validation-scheme"></a>

* A new libprocess TLS flag `--hostname_validation_scheme` along with the corresponding environment variable `LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME`
  has been added. Using this flag, users can configure the way libprocess performs hostname validation for TLS connections.
  See [`docs/ssl`](ssl.md) for details.

<a name="1-9-x-client-certificate-verification"></a>

* The semantics of the libprocess environment variables `LIBPROCESS_SSL_VERIFY_CERT` and `LIBPROCESS_SSL_REQUIRE_CERT` have been slightly updated such that
  the former now only applies to client-mode and the latter only to server-mode connections. As part of this re-adjustment, the following two changes have
  been introduced that might require changes for operators running Mesos in unusual TLS configurations.
  * Anonymous ciphers can not be used anymore when `LIBPROCESS_SSL_VERIFY_CERT` is set to true. This is because the use of anonymous ciphers enables
    a malicious attacker to bypass certificate verification by choosing a certificate-less cipher.
    Users that rely on anonymous ciphers being available should make sure that `LIBPROCESS_SSL_VERIFY_CERT` is set to false.
  * For incoming connections, certificates are not verified unless `LIBPROCESS_SSL_REQUIRE_CERT` is set to true.
    This is because verifying the certificate can lead to false negatives, where a connection is aborted even though presenting no certificate at all
    would have been successfull. Users that rely on incoming connection requests presenting valid TLS certificates should make sure that
    the `LIBPROCESS_SSL_REQUIRE_CERT` option is set to true.

<a name="1-9-x-configurable-ipc"></a>

* The Mesos containerizer now supports configurable IPC namespace and /dev/shm. Container can be configured to have a private IPC namespace and /dev/shm or share them from its parent via the field `LinuxInfo.ipc_mode`, and the size of its private /dev/shm is also configurable via the field `LinuxInfo.shm_size`. Operators can control whether it is allowed to share host's IPC namespace and /dev/shm with top level containers via the agent flag `--disallow_sharing_agent_ipc_namespace`, and specify the default size of the /dev/shm for the container which has a private /dev/shm via the agent flag `--default_container_shm_size`.

<a name="1-9-x-update-quota"></a>

* The `SET_QUOTA` and `REMOVE QUOTA` master calls are deprecated in favor of a new `UPDATE_QUOTA` master call.

<a name="1-9-x-quota-guarantees"></a>

* Prior to Mesos 1.9, the quota related APIs only exposed quota "guarantees" which ensured a minimum amount of resources would be available to a role. Setting guarantees also set implicit quota limits. In Mesos 1.9+, quota limits are now exposed directly.
  * Quota guarantees are now deprecated in favor of using only quota limits. Enforcement of quota guarantees required that Mesos holds back enough resources to meet all of the unsatisfied quota guarantees. Since Mesos is moving towards an optimistic offer model (to improve multi-role / multi- scheduler scalability, see MESOS-1607), it will become no longer possible to enforce quota guarantees by holding back resources. In such a model, quota limits are simple to enforce, but quota guarantees would require a complex "effective limit" propagation model to leave space for unsatisfied guarantees.
  * For these reasons, quota guarantees, while still functional in Mesos 1.9, are now deprecated. A combination of limits and priority based preemption will be simpler in an optimistic offer model.

## Upgrading from 1.7.x to 1.8.x ##

<a name="1-8-x-linux-seccomp-isolator"></a>

* A new [`linux/seccomp`](isolators/linux-seccomp.md) isolator has been added. The isolator supports the following new agent flags:
  * `--seccomp_config_dir` specifies the directory path of the Seccomp profiles.
  * `--seccomp_profile_name` specifies the path of the default Seccomp profile relative to the `seccomp_config_dir`.

## Upgrading from 1.6.x to 1.7.x ##

<a name="1-7-x-linux-devices-isolator"></a>

* A new [`linux/devices`](isolators/linux-devices.md) isolator has been
  added. This isolator automatically populates containers with devices
  that have been whitelisted with the `--allowed_devices` agent flag.

<a name="1-7-x-auto-load-subsystems"></a>

* A new option `cgroups/all` has been added to the agent flag `--isolation`. This allows cgroups isolator to automatically load all the local enabled cgroups subsystems. If this option is specified in the agent flag `--isolation` along with other cgroups related options (e.g., `cgroups/cpu`), those options will be just ignored.

<a name="1-7-x-container-specific-cgroups-mounts"></a>

* Added container-specific cgroups mounts under `/sys/fs/cgroup` to containers with image launched by Mesos containerizer.

<a name="1-7-x-volume-mode-support"></a>

* Previously the `HOST_PATH`, `SANDBOX_PATH`, `IMAGE`, `SECRET`, and `DOCKER_VOLUME` volumes were always mounted for container in read-write mode, i.e., the `Volume.mode` field was not honored. Now we will mount these volumes based on the `Volume.mode` field so framework can choose to mount the volume for the container in either read-write mode or read-only mode.

<a name="1-7-x-create-disk"></a>

* To simplify the API for CSI-backed disk resources, the following operations and corresponding ACLs have been introduced to replace the experimental `CREATE_VOLUME`, `CREATE_BLOCK`, `DESTROY_VOLUME` and `DESTROY_BLOCK` operations:
  * `CREATE_DISK` to create a `MOUNT` or `BLOCK` disk resource from a `RAW` disk resource. The `CreateMountDisk` and `CreateBlockDisk` ACLs control which principals are allowed to create `MOUNT` or `BLOCK` disks for which roles.
  * `DESTROY_DISK` to reclaim a `MOUNT` or `BLOCK` disk resource back to a `RAW` disk resource. The `DestroyMountDisk` and `DestroyBlockDisk` ACLs control which principals are allowed to reclaim `MOUNT` or `BLOCK` disks for which roles.

<a name="1-7-x-resource-provider-acls"></a>

* A new `ViewResourceProvider` ACL has been introduced to control which principals are allowed to call the `GET_RESOURCE_PROVIDERS` agent API.

<a name="1-7-x-enforce-container-ports"></a>

* A new [`--enforce_container_ports`](configuration/agent.md#enforce_container_ports) flag has been added to toggle whether the [`network/ports`](isolators/network-ports.md) isolator should enforce TCP ports usage limits.

<a name="1-7-x-gc-non-executor-container-sandboxes"></a>

* A new [`--gc_non_executor_container_sandboxes`](configuration/agent.md#gc_non_executor_container_sandboxes)
  agent flag has been added to garbage collect the sandboxes of nested
  containers, which includes the tasks groups launched by the default executor.
  We recommend enabling the flag if you have frameworks that launch multiple
  task groups on the same default executor instance.

<a name="1-7-x-network-cni-root-dir-persist"></a>

* A new [`--network_cni_root_dir_persist`](configuration/agent.md#network_cni_root_dir_persist) flag has been added to toggle whether the [`network/cni`](cni.md) isolator should persist the network information across reboots.

<a name="1-7-x-container-logger"></a>

* `ContainerLogger` module interface has been changed. The `prepare()` method now takes `ContainerID` and `ContainerConfig` instead.

<a name="1-7-x-isolator-recover"></a>

* `Isolator::recover()` has been updated to take an `std::vector` instead of `std::list` of container states.

<a name="1-7-x-json-serialization"></a>

* As a result of adapting rapidjson for performance improvement, all JSON endpoints serialize differently while still conforming to the ECMA-404 spec for JSON. This means that if a client has a JSON de-serializer that conforms to ECMA-404 they will see no change. Otherwise, they may break. As an example, Mesos would previously serialize '/' as '\/', but the spec does not require the escaping and rapidjson does not escape '/'.

## Upgrading from 1.5.x to 1.6.x ##

<a name="1-6-x-grpc-requirement"></a>

* gRPC version 1.10+ is required to build Mesos when enabling gRPC-related features. Please upgrade your gRPC library if you are using an unbundled one.

<a name="1-6-x-csi-support"></a>

* CSI v0.2 is now supported as experimental. Due to the incompatibility between CSI v0.1 and v0.2, the experimental support for CSI v0.1 is removed, and the operator must remove all storage local resource providers within an agent before upgrading the agent. NOTE: This is a **breaking change** for storage local resource providers.

<a name="1-6-x-fetcher-stall-timeout"></a>

* A new agent flag `--fetcher_stall_timeout` has been added. This flag specifies the amount of time for the container image and artifact fetchers to wait before aborting a stalled download (i.e., the speed keeps below one byte per second). NOTE: This flag only applies when downloading data from the net and does not apply to HDFS.

<a name="1-6-x-disk-profile-adaptor"></a>

* The disk profile adaptor module has been changed to support CSI v0.2, and its header file has been renamed to be consistent with other modules. See `disk_profile_adaptor.hpp` for interface changes.

<a name="1-6-x-xfs-kill-containers"></a>

* A new agent flag `--xfs_kill_containers` has been added. By setting this flag, the [`disk/xfs`](isolators/disk-xfs.md) isolator
  will now kill containers that exceed the disk limit.

## Upgrading from 1.4.x to 1.5.x ##

<a name="1-5-x-task-starting"></a>

* The built-in executors will now send a `TASK_STARTING` status update for
  every task they've successfully received and are about to start.
  The possibility of any executor sending this update has been documented since
  the beginning of Mesos, but prior to this version the built-in executors did
  not actually send it. This means that all schedulers using one of the built-in
  executors must be upgraded to expect `TASK_STARTING` updates before upgrading
  Mesos itself.

<a name="1-5-x-task-resource-limitation"></a>

* A new field, `limitation`, was added to the `TaskStatus` message. This
  field is a `TaskResourceLimitation` message that describes the resources
  that caused a task to fail with a resource limitation reason.

<a name="1-5-x-network-ports-isolator"></a>

* A new [`network/ports`](isolators/network-ports.md) isolator has been added. The isolator supports the following new agent flags:
  * `--container_ports_watch_interval` specifies the interval at which the isolator reconciles port assignments.
  * `--check_agent_port_range_only` excludes ports outside the agent's range from port reconciliation.

<a name="1-5-x-executor-secret-key"></a>

* Agent flag `--executor_secret_key` has been deprecated. Operators should use `--jwt_secret_key` instead.

<a name="1-5-x-relative-disk-source-root-path"></a>

* The fields `Resource.disk.source.path.root` and `Resource.disk.source.mount.root` can now be set to relative paths to an agent's work directory. The containerizers will interpret the paths based on the `--work_dir` flag on an agent.

<a name="1-5-x-get-containers"></a>

* The agent operator API call `GET_CONTAINERS` has been updated to support listing nested or standalone containers. One can specify the following fields in the request:
  * `show_nested`: Whether to show nested containers.
  * `show_standalone`: Whether to show standalone containers.

<a name="1-5-x-reconfiguration-policy"></a>

* A new agent flag `--reconfiguration_policy` has been added. By setting the value of this flag to `additive`,
  operators can allow the agent to be restarted with increased resources without requiring the agent ID to be
  changed. Note that if this feature is used, the master version is required to be >= 1.5 as well.

<a name="1-5-x-protobuf-requirement"></a>

* Protobuf version 3+ is required to build Mesos. Please upgrade your Protobuf library if you are using an unbundled one.

<a name="1-5-x-log-reader-catchup"></a>

* A new `catchup()` method has been added to the replicated log reader API. The method allows to catch-up positions missing in the local non-leading replica to allow safe eventually consistent reads from it. Note about backwards compatibility: In order for the feature to work correctly in presence of log truncations all log replicas need to be updated.

## Upgrading from 1.3.x to 1.4.x ##

<a name="1-4-x-ambient-capabilities"></a>

* If the `mesos-agent` kernel supports ambient capabilities (Linux 4.3 or later), the capabilities specified in the `LinuxInfo.effective_capabilities` message will be made ambient in the container task.

<a name="1-4-x-bounding-capabilities"></a>

* Explicitly setting the bounding capabilities of a task independently of the effective capabilities is now supported. Frameworks can specify the task bounding capabilities by using the `LinuxInfo.bounding_capabilities` message. Operators can specify the default bounding capabilities using the agent `--bounding_capabilities` flag. This flag also specifies the maximum bounding set that a framework is allowed to specify.

<a name="1-4-x-agent-recovery"></a>

* Agent is now allowed to recover its agent ID post a host reboot. This prevents the unnecessary discarding of agent ID by prior Mesos versions. Notes about backwards compatibility:
  * In case the agent's recovery runs into agent info mismatch which may happen due to resource change associated with reboot, it'll fall back to recovering as a new agent (existing behavior).
  * In other cases such as checkpointed resources (e.g. persistent volumes) being incompatible with the agent's resources the recovery will still fail (existing behavior).

<a name="1-4-x-linuxinfo-capabilities"></a>

* The `LinuxInfo.capabilities` field has been deprecated in favor of `LinuxInfo.effective_capabilities`.

<a name="1-4-x-agent-capabilities-flags"></a>

* Changes to capability-related agent flags:
  * The agent `--effective_capabilities` flag has been added to specify the default effective capability set for tasks.
  * The agent `--bounding_capabilities` flag has been added to specify the default bounding capability set for tasks.
  * The agent `--allowed-capabilities` flag has been deprecated in favor of `--effective_capabilities`.

<a name="1-4-x-allocator-update-slave"></a>

* The semantics of the optional resource argument passed in `Allocator::updateSlave` was change. While previously the passed value denoted a new amount of oversubscribed (revocable) resources on the agent, it now denotes the new amount of total resources on the agent. This requires custom allocator implementations to update their interpretation of the passed value.

<a name="1-4-x-xfs-no-enforce"></a>

* The XFS Disk Isolator now supports the `--no-enforce_container_disk_quota` option to efficiently measure disk resource usage without enforcing any usage limits.

<a name="1-4-x-mesos-library"></a>

* The `Resources` class in the internal Mesos C++ library changed its behavior to only support post-`RESERVATION_REFINEMENT` format. If a framework is using this internal utility, it is likely to break if the `RESERVATION_REFINEMENT` capability is not enabled.

<a name="1-4-x-update-minimal-docker-version"></a>

* To specify the `--type=container` option for the `docker inspect <container_name>` command, the minimal supported Docker version has been updated from 1.0.0 to 1.8.0 since Docker supported `--type=container` for the `docker inspect` command starting from 1.8.0.

## Upgrading from 1.2.x to 1.3.x ##

<a name="1-3-x-disallow-old-agents"></a>

* The master will no longer allow 0.x agents to register. Interoperability between 1.1+ masters and 0.x agents has never been supported; however, it was not explicitly disallowed, either. Starting with this release of Mesos, registration attempts by 0.x agents will be ignored.

<a name="1-3-x-setquota-removequota-acl"></a>

* Support for deprecated ACLs `set_quotas` and `remove_quotas` has been removed from the local authorizer. Before upgrading the Mesos binaries, consolidate the ACLs used under `set_quotas` and `remove_quotes` under their replacement ACL `update_quotas`. After consolidation of the ACLs, the binaries could be safely replaced.

<a name="1-3-x-shutdown-framework-acl"></a>

* Support for deprecated ACL `shutdown_frameworks` has been removed from the local authorizer. Before upgrading the Mesos binaries, replace all instances of the ACL `shutdown_frameworks` with the newer ACL `teardown_frameworks`. After updating the ACLs, the binaries can be safely replaced.

<a name="1-3-x-multi-role-support"></a>
<a name="1-3-x-framework-info-role"></a>

* Support for multi-role frameworks deprecates the `FrameworkInfo.role` field in favor of `FrameworkInfo.roles` and the `MULTI_ROLE` capability. Frameworks using the new field can continue to use a single role.

<a name="1-3-x-endpoints-roles"></a>

* Support for multi-role frameworks means that the framework `role` field in the master and agent endpoints is deprecated in favor of `roles`. Any tooling parsing endpoint information and relying on the role field needs to be updated before multi-role frameworks can be safely run in the cluster.

<a name="1-3-x-allocator-interface-change"></a>

* Implementors of allocator modules have to provide new implementation functionality to satisfy the `MULTI_ROLE` framework capability. Also, the interface has changed.

<a name="1-3-x-executor-authentication"></a>

* New Agent flags `authenticate_http_executors` and `executor_secret_key`: Used to enable required HTTP executor authentication and set the key file used for generation and authentication of HTTP executor tokens. Note that enabling these flags after upgrade is disruptive to HTTP executors that were launched before the upgrade. For more information on the recommended upgrade procedure when enabling these flags, see the [authentication documentation](authentication.md).

In order to upgrade a running cluster:

1. Rebuild and install any modules so that upgraded masters/agents/schedulers can use them.
2. Install the new master binaries and restart the masters.
3. Install the new agent binaries and restart the agents.
4. Upgrade the schedulers by linking the latest native library / jar / egg (if necessary).
5. Restart the schedulers.
6. Upgrade the executors by linking the latest native library / jar / egg (if necessary).

## Upgrading from 1.1.x to 1.2.x ##

<a name="1-2-1-disallow-old-agents"></a>

* In Mesos 1.2.1, the master will no longer allow 0.x agents to register. Interoperability between 1.1+ masters and 0.x agents has never been supported; however, it was not explicitly disallowed, either. Starting with Mesos 1.2.1, registration attempts by 0.x agents will be ignored. **NOTE:** This applies only when upgrading to Mesos 1.2.1. Mesos 1.2.0 does not implement this behavior.

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

* Mesos 1.1 adds an `offeredResources` argument to the `Allocator::updateAllocation()` method. It is used to indicate the resources that the operations passed to `updateAllocation()` are applied to. [MESOS-4431](https://issues.apache.org/jira/browse/MESOS-4431) (particularly [/r/45961/](https://reviews.apache.org/r/45961/)) has more details on the motivation.

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

* By default, executors will no longer inherit environment variables from the agent. The operator can still use the `--executor_environment_variables` flag on the agent to explicitly specify what environment variables the executors will get. Mesos generated environment variables (i.e., `$MESOS_`, `$LIBPROCESS_`) will not be affected. If `$PATH` is not specified for an executor, a default value `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin` will be used.

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
  * The `update_quotas` ACL cannot be used in combination with either the `set_quotas` or `remove_quotas` ACL. The local authorizer will produce an error in such a case;
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

* The `--authenticate_http` flag has been deprecated in favor of `--authenticate_http_readwrite`. Setting `--authenticate_http_readwrite` will now enable authentication for all endpoints which previously had authentication support. These happen to be the endpoints which allow modification of the cluster state, or "read-write" endpoints. Note that `/logging/toggle`, `/profiler/start`, `/profiler/stop`, `/maintenance/schedule`, `/machine/up`, and `/machine/down` previously did not have authentication support, but in 1.0 if either `--authenticate_http` or `--authenticate_http_readwrite` is set, those endpoints will now require authentication. A new flag has also been introduced, `--authenticate_http_readonly`, which enables authentication for endpoints which support authentication and do not allow modification of the state of the cluster, like `/state` or `/flags`.

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

  * /state.json becomes /state
  * /monitor/statistics.json becomes /monitor/statistics

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
