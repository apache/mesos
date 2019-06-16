---
title: Apache Mesos - Observability Metrics
layout: documentation
---


# Mesos Observability Metrics

This document describes the observability metrics provided by Mesos master and
agent nodes. This document also provides some initial guidance on which metrics
you should monitor to detect abnormal situations in your cluster.


## Overview

Mesos master and agent nodes report a set of statistics and metrics that enable
cluster operators to monitor resource usage and detect abnormal situations early. The
information reported by Mesos includes details about available resources, used
resources, registered frameworks, active agents, and task state. You can use
this information to create automated alerts and to plot different metrics over
time inside a monitoring dashboard.

Metric information is not persisted to disk at either master or agent
nodes, which means that metrics will be reset when masters and agents
are restarted. Similarly, if the current leading master fails and a new
leading master is elected, metrics at the new master will be reset.


## Metric Types

Mesos provides two different kinds of metrics: counters and gauges.

**Counters** keep track of discrete events and are monotonically increasing. The
value of a metric of this type is always a natural number. Examples include the
number of failed tasks and the number of agent registrations. For some metrics
of this type, the rate of change is often more useful than the value itself.

**Gauges** represent an instantaneous sample of some magnitude. Examples include
the amount of used memory in the cluster and the number of connected agents. For
some metrics of this type, it is often useful to determine whether the value is
above or below a threshold for a sustained period of time.

The tables in this document indicate the type of each available metric.


## Master Nodes

Metrics from each master node are available via the
[/metrics/snapshot](endpoints/metrics/snapshot.md) master endpoint.  The response
is a JSON object that contains metrics names and values as key-value pairs.

### Observability metrics

This section lists all available metrics from Mesos master nodes grouped by
category.

#### Resources

The following metrics provide information about the total resources available in
the cluster and their current usage. High resource usage for sustained periods
of time may indicate that you need to add capacity to your cluster or that a
framework is misbehaving.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/cpus_percent</code>
  </td>
  <td>Percentage of allocated CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/cpus_used</code>
  </td>
  <td>Number of allocated CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/cpus_total</code>
  </td>
  <td>Number of CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/cpus_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/cpus_revocable_total</code>
  </td>
  <td>Number of revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/cpus_revocable_used</code>
  </td>
  <td>Number of allocated revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_percent</code>
  </td>
  <td>Percentage of allocated disk space</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_used</code>
  </td>
  <td>Allocated disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_total</code>
  </td>
  <td>Disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable disk space</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_revocable_total</code>
  </td>
  <td>Revocable disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/disk_revocable_used</code>
  </td>
  <td>Allocated revocable disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_percent</code>
  </td>
  <td>Percentage of allocated GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_used</code>
  </td>
  <td>Number of allocated GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_total</code>
  </td>
  <td>Number of GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_revocable_total</code>
  </td>
  <td>Number of revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/gpus_revocable_used</code>
  </td>
  <td>Number of allocated revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_percent</code>
  </td>
  <td>Percentage of allocated memory</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_used</code>
  </td>
  <td>Allocated memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_total</code>
  </td>
  <td>Memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable memory</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_revocable_total</code>
  </td>
  <td>Revocable memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/mem_revocable_used</code>
  </td>
  <td>Allocated revocable memory in MB</td>
  <td>Gauge</td>
</tr>
</table>

#### Master

The following metrics provide information about whether a master is currently
elected and how long it has been running. A cluster with no elected master
for sustained periods of time indicates a malfunctioning cluster. This
points to either leadership election issues (so check the connection to
ZooKeeper) or a flapping Master process. A low uptime value indicates that the
master has restarted recently.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/elected</code>
  </td>
  <td>Whether this is the elected master</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/uptime_secs</code>
  </td>
  <td>Uptime in seconds</td>
  <td>Gauge</td>
</tr>
</table>

#### System

The following metrics provide information about the resources available on this
master node and their current usage. High resource usage in a master node for
sustained periods of time may degrade the performance of the cluster.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>system/cpus_total</code>
  </td>
  <td>Number of CPUs available in this master node</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_15min</code>
  </td>
  <td>Load average for the past 15 minutes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_5min</code>
  </td>
  <td>Load average for the past 5 minutes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_1min</code>
  </td>
  <td>Load average for the past minute</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/mem_free_bytes</code>
  </td>
  <td>Free memory in bytes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/mem_total_bytes</code>
  </td>
  <td>Total memory in bytes</td>
  <td>Gauge</td>
</tr>
</table>

#### Agents

The following metrics provide information about agent events, agent counts, and
agent states. A low number of active agents may indicate that agents are
unhealthy or that they are not able to connect to the elected master.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/slave_registrations</code>
  </td>
  <td>Number of agents that were able to cleanly re-join the cluster and
      connect back to the master after the master is disconnected.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals</code>
  </td>
  <td>Number of agent removed for various reasons, including maintenance</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_reregistrations</code>
  </td>
  <td>Number of agent re-registrations</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_unreachable_scheduled</code>
  </td>
  <td>Number of agents which have failed their health check and are scheduled
      to be marked unreachable. They will not be marked unreachable immediately due to the Agent
      Removal Rate-Limit, but <code>master/slave_unreachable_completed</code>
      will start increasing as they do get removed.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_unreachable_canceled</code>
  </td>
  <td>Number of times that an agent was due to be marked unreachable but this
      transition was cancelled. This happens when the agent removal rate limit
      is enabled and the agent sends a <code>PONG</code> response message to the
      master before the rate limit allows the agent to be marked unreachable.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_unreachable_completed</code>
  </td>
  <td>Number of agents that were marked as unreachable because they failed
      health checks. These are agents which were not heard from despite the
      agent-removal rate limit, and have been marked as unreachable in the
      master's agent registry.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slaves_active</code>
  </td>
  <td>Number of active agents</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_connected</code>
  </td>
  <td>Number of connected agents</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_disconnected</code>
  </td>
  <td>Number of disconnected agents</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_inactive</code>
  </td>
  <td>Number of inactive agents</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_unreachable</code>
  </td>
  <td>Number of unreachable agents. Unreachable agents are periodically
      garbage collected from the registry, which will cause this value to
      decrease.</td>
  <td>Gauge</td>
</tr>
</table>

#### Frameworks

The following metrics provide information about the registered frameworks in the
cluster. No active or connected frameworks may indicate that a scheduler is not
registered or that it is misbehaving.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/frameworks_active</code>
  </td>
  <td>Number of active frameworks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/frameworks_connected</code>
  </td>
  <td>Number of connected frameworks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/frameworks_disconnected</code>
  </td>
  <td>Number of disconnected frameworks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/frameworks_inactive</code>
  </td>
  <td>Number of inactive frameworks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/outstanding_offers</code>
  </td>
  <td>Number of outstanding resource offers</td>
  <td>Gauge</td>
</tr>
</table>

The following metrics are added for each framework which registers with the
master, in order to provide detailed information about the behavior of the
framework. The framework name is percent-encoded before creating these metrics;
the actual name can be recovered by percent-decoding.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/subscribed</code>
  </td>
  <td>Whether or not this framework is currently subscribed</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/calls</code>
  </td>
  <td>Total number of calls sent by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/calls/&lt;CALL_TYPE&gt;</code>
  </td>
  <td>Number of each type of call sent by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/events</code>
  </td>
  <td>Total number of events sent to this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/events/&lt;EVENT_TYPE&gt;</code>
  </td>
  <td>Number of each type of event sent to this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/operations</code>
  </td>
  <td>Total number of offer operations performed by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/operations/&lt;OPERATION_TYPE&gt;</code>
  </td>
  <td>Number of each type of offer operation performed by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/tasks/active/&lt;TASK_STATE&gt;</code>
  </td>
  <td>Number of this framework's tasks currently in each active task state</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/tasks/terminal/&lt;TASK_STATE&gt;</code>
  </td>
  <td>Number of this framework's tasks which have transitioned into each terminal task state</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/offers/sent</code>
  </td>
  <td>Number of offers sent to this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/offers/accepted</code>
  </td>
  <td>Number of offers accepted by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/offers/declined</code>
  </td>
  <td>Number of offers explicitly declined by this framework</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/offers/rescinded</code>
  </td>
  <td>Number of offers sent to this framework which were subsequently rescinded</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/frameworks/&lt;ENCODED_FRAMEWORK_NAME&gt;/&lt;FRAMEWORK_ID&gt;/roles/&lt;ROLE_NAME&gt;/suppressed</code>
  </td>
  <td>For each of the framework's subscribed roles, whether or not offers for that role are currently suppressed</td>
  <td>Gauge</td>
</tr>
</table>

#### Tasks

The following metrics provide information about active and terminated tasks. A
high rate of lost tasks may indicate that there is a problem with the cluster.
The task states listed here match those of the task state machine.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/tasks_error</code>
  </td>
  <td>Number of tasks that were invalid</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/tasks_failed</code>
  </td>
  <td>Number of failed tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/tasks_finished</code>
  </td>
  <td>Number of finished tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/tasks_killed</code>
  </td>
  <td>Number of killed tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/tasks_killing</code>
  </td>
  <td>Number of tasks currently being killed</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/tasks_lost</code>
  </td>
  <td>Number of lost tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/tasks_running</code>
  </td>
  <td>Number of running tasks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/tasks_staging</code>
  </td>
  <td>Number of staging tasks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/tasks_starting</code>
  </td>
  <td>Number of starting tasks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/tasks_unreachable</code>
  </td>
  <td>Number of unreachable tasks</td>
  <td>Gauge</td>
</tr>
</table>

#### Operations

The following metrics provide information about offer operations on the master.

Below, `OPERATION_TYPE` refers to any one of `reserve`, `unreserve`, `create`,
`destroy`, `grow_volume`, `shrink_volume`, `create_disk` or `destroy_disk`.

NOTE: The counter for terminal operation states can over-count over time. In
particular if an agent contained unacknowledged terminal status updates when
it was marked gone or marked unreachable, these operations will be double-counted
as both their original state and `OPERATION_GONE`/`OPERATION_UNREACHABLE`.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/operations/total</code>
  </td>
  <td>Total number of operations known to this master</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/operations/&lt;OPERATION_STATE&gt;</code>
  </td>
  <td>Number of operations in the given non-terminal state (`pending`, `recovering` or `unreachable`)</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/operations/&lt;OPERATION_STATE&gt;</code>
  </td>
  <td>Number of operations in the given terminal state (`finished`, `error`, `dropped` or `gone_by_operator`)</td>
  <td>Counter</td>
</tr>

<tr>
  <td>
  <code>master/operations/&lt;OPERATION_TYPE&gt;/total</code>
  </td>
  <td>Total number of operations with the given type known to this master</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/operations/&lt;OPERATION_TYPE&gt;/&lt;OPERATION_STATE&gt;</code>
  </td>
  <td>Number of operations with the given type in the given non-terminal state (`pending`, `recovering` or `unreachable`)</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/operations/&lt;OPERATION_TYPE&gt;/&lt;OPERATION_STATE&gt;</code>
  </td>
  <td>Number of operations with the given type in the given state (`finished`, `error`, `dropped` or `gone_by_operator`)</td>
  <td>Counter</td>
</tr>
</table>

#### Messages

The following metrics provide information about messages between the master and
the agents and between the framework and the executors. A high rate of dropped
messages may indicate that there is a problem with the network.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/invalid_executor_to_framework_messages</code>
  </td>
  <td>Number of invalid executor to framework messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/invalid_framework_to_executor_messages</code>
  </td>
  <td>Number of invalid framework to executor messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/invalid_operation_status_update_acknowledgements</code>
  </td>
  <td>Number of invalid operation status update acknowledgements</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/invalid_status_update_acknowledgements</code>
  </td>
  <td>Number of invalid status update acknowledgements</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/invalid_status_updates</code>
  </td>
  <td>Number of invalid status updates</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/dropped_messages</code>
  </td>
  <td>Number of dropped messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_authenticate</code>
  </td>
  <td>Number of authentication messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_deactivate_framework</code>
  </td>
  <td>Number of framework deactivation messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_decline_offers</code>
  </td>
  <td>Number of offers declined</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_executor_to_framework</code>
  </td>
  <td>Number of executor to framework messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_exited_executor</code>
  </td>
  <td>Number of terminated executor messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_framework_to_executor</code>
  </td>
  <td>Number of messages from a framework to an executor</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_kill_task</code>
  </td>
  <td>Number of kill task messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_launch_tasks</code>
  </td>
  <td>Number of launch task messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_operation_status_update_acknowledgement</code>
  </td>
  <td>Number of operation status update acknowledgement messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_reconcile_operations</code>
  </td>
  <td>Number of reconcile operations messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_reconcile_tasks</code>
  </td>
  <td>Number of reconcile task messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_register_framework</code>
  </td>
  <td>Number of framework registration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_register_slave</code>
  </td>
  <td>Number of agent registration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_reregister_framework</code>
  </td>
  <td>Number of framework re-registration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_reregister_slave</code>
  </td>
  <td>Number of agent re-registration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_resource_request</code>
  </td>
  <td>Number of resource request messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_revive_offers</code>
  </td>
  <td>Number of offer revival messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_status_update</code>
  </td>
  <td>Number of status update messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_status_update_acknowledgement</code>
  </td>
  <td>Number of status update acknowledgement messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_unregister_framework</code>
  </td>
  <td>Number of framework unregistration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_unregister_slave</code>
  </td>
  <td>Number of agent unregistration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_update_slave</code>
  </td>
  <td>Number of update agent messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/recovery_slave_removals</code>
  </td>
  <td>Number of agents not reregistered during master failover</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_registered</code>
  </td>
  <td>Number of agents removed when new agents registered at the same address</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_unhealthy</code>
  </td>
  <td>Number of agents failed due to failed health checks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_unregistered</code>
  </td>
  <td>Number of agents unregistered</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_framework_to_executor_messages</code>
  </td>
  <td>Number of valid framework to executor messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_operation_status_update_acknowledgements</code>
  </td>
  <td>Number of valid operation status update acknowledgement messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_status_update_acknowledgements</code>
  </td>
  <td>Number of valid status update acknowledgement messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_status_updates</code>
  </td>
  <td>Number of valid status update messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/task_lost/source_master/reason_invalid_offers</code>
  </td>
  <td>Number of tasks lost due to invalid offers</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/task_lost/source_master/reason_slave_removed</code>
  </td>
  <td>Number of tasks lost due to agent removal</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/task_lost/source_slave/reason_executor_terminated</code>
  </td>
  <td>Number of tasks lost due to executor termination</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_executor_to_framework_messages</code>
  </td>
  <td>Number of valid executor to framework messages</td>
  <td>Counter</td>
</tr>
</table>

#### Event queue

The following metrics provide information about different types of events in the
event queue.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/event_queue_dispatches</code>
  </td>
  <td>Number of dispatches in the event queue</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/event_queue_http_requests</code>
  </td>
  <td>Number of HTTP requests in the event queue</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/event_queue_messages</code>
  </td>
  <td>Number of messages in the event queue</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/operator_event_stream_subscribers</code>
  </td>
  <td>Number of subscribers to the operator event stream</td>
  <td>Gauge</td>
</tr>
</table>

#### Registrar

The following metrics provide information about read and write latency to the
agent registrar.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>registrar/state_fetch_ms</code>
  </td>
  <td>Registry read latency in ms </td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms</code>
  </td>
  <td>Registry write latency in ms </td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/max</code>
  </td>
  <td>Maximum registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/min</code>
  </td>
  <td>Minimum registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p50</code>
  </td>
  <td>Median registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p90</code>
  </td>
  <td>90th percentile registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p95</code>
  </td>
  <td>95th percentile registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p99</code>
  </td>
  <td>99th percentile registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p999</code>
  </td>
  <td>99.9th percentile registry write latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/state_store_ms/p9999</code>
  </td>
  <td>99.99th percentile registry write latency in ms</td>
  <td>Gauge</td>
</tr>
</table>

#### Replicated log

The following metrics provide information about the replicated log underneath
the registrar, which is the persistent store for masters.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>registrar/log/recovered</code>
  </td>
  <td>
    Whether the replicated log for the registrar has caught up with the other
    masters in the cluster. A cluster is operational as long as a quorum of
    "recovered" masters is available in the cluster.
  </td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>registrar/log/ensemble_size</code>
  </td>
  <td>
    The number of masters in the ensemble (cluster) that the current master
    communicates with (including itself) to form the replicated log quorum.
    It's imperative that this number is always less than `--quorum * 2` to
    prevent split-brain. It's also important that it should be greater than
    or equal to `--quorum` to maintain availability.
  </td>
  <td>Gauge</td>
</tr>
</table>

#### Allocator

The following metrics provide information about performance
and resource allocations in the allocator.

<table class="table table-stripped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms</code>
  </td>
  <td>Time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/count</code>
  </td>
  <td>Number of allocation algorithm time measurements in the window</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/max</code>
  </td>
  <td>Maximum time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/min</code>
  </td>
  <td>Minimum time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p50</code>
  </td>
  <td>Median time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p90</code>
  </td>
  <td>90th percentile of time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p95</code>
  </td>
  <td>95th percentile of time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p99</code>
  </td>
  <td>99th percentile of time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p999</code>
  </td>
  <td>99.9th percentile of time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_ms/p9999</code>
  </td>
  <td>99.99th percentile of time spent in allocation algorithm in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_runs</code>
  </td>
  <td>Number of times the allocation algorithm has run</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms</code>
  </td>
  <td>Allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/count</code>
  </td>
  <td>Number of allocation batch latency measurements in the window</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/max</code>
  </td>
  <td>Maximum allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/min</code>
  </td>
  <td>Minimum allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p50</code>
  </td>
  <td>Median allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p90</code>
  </td>
  <td>90th percentile allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p95</code>
  </td>
  <td>95th percentile allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p99</code>
  </td>
  <td>99th percentile allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p999</code>
  </td>
  <td>99.9th percentile allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/allocation_run_latency_ms/p9999</code>
  </td>
  <td>99.99th percentile allocation batch latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/roles/<i>&lt;role&gt;</i>/shares/dominant</code>
  </td>
  <td>Dominant <i>resource</i> share for the <i>role</i>, exposed as a percentage (0.0-1.0)</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/event_queue_dispatches</code>
  </td>
  <td>Number of dispatch events in the event queue</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/offer_filters/roles/<i>&lt;role&gt;</i>/active</code>
  </td>
  <td>Number of active offer filters for all frameworks within the <i>role</i></td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/quota/roles/<i>&lt;role&gt;</i>/resources/<i>&lt;resource&gt;</i>/offered_or_allocated</code>
  </td>
  <td>Amount of <i>resource</i>s considered offered or allocated towards
      a <i>role</i>'s quota guarantee</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/quota/roles/<i>&lt;role&gt;</i>/resources/<i>&lt;resource&gt;</i>/guarantee</code>
  </td>
  <td>Amount of <i>resource</i>s guaranteed for a <i>role</i> via quota</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/cpus/offered_or_allocated</code>
  </td>
  <td>Number of CPUs offered or allocated</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/cpus/total</code>
  </td>
  <td>Number of CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/disk/offered_or_allocated</code>
  </td>
  <td>Allocated or offered disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/disk/total</code>
  </td>
  <td>Total disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/mem/offered_or_allocated</code>
  </td>
  <td>Allocated or offered memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>allocator/mesos/resources/mem/total</code>
  </td>
  <td>Total memory in MB</td>
  <td>Gauge</td>
</tr>
</table>

### Basic Alerts

This section lists some examples of basic alerts that you can use to detect
abnormal situations in a cluster.

#### master/uptime_secs is low

The master has restarted.

#### master/uptime_secs < 60 for sustained periods of time

The cluster has a flapping master node.

#### master/tasks_lost is increasing rapidly

Tasks in the cluster are disappearing. Possible causes include hardware
failures, bugs in one of the frameworks, or bugs in Mesos.

#### master/slaves_active is low

Agents are having trouble connecting to the master.

#### master/cpus_percent > 0.9 for sustained periods of time

Cluster CPU utilization is close to capacity.

#### master/mem_percent > 0.9 for sustained periods of time

Cluster memory utilization is close to capacity.

#### master/elected is 0 for sustained periods of time

No master is currently elected.




## Agent Nodes

Metrics from each agent node are available via the
[/metrics/snapshot](endpoints/metrics/snapshot.md) agent endpoint.  The response
is a JSON object that contains metrics names and values as key-value pairs.

### Observability Metrics

This section lists all available metrics from Mesos agent nodes grouped by
category.

#### Resources

The following metrics provide information about the total resources available in
the agent and their current usage.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>containerizer/fetcher/cache_size_total_bytes</code>
  </td>
  <td>The configured maximum size of the fetcher cache in bytes. This value is
  constant for the life of the Mesos agent.</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/fetcher/cache_size_used_bytes</code>
  </td>
  <td>The current amount of data stored in the fetcher cache in bytes.</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>gc/path_removals_failed</code>
  </td>
  <td>Number of times the agent garbage collection process has failed to remove a sandbox path.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>gc/path_removals_pending</code>
  </td>
  <td>Number of sandbox paths that are currently pending agent garbage collection.</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>gc/path_removals_succeeded</code>
  </td>
  <td>Number of sandbox paths the agent successfully removed.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_percent</code>
  </td>
  <td>Percentage of allocated CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_used</code>
  </td>
  <td>Number of allocated CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_total</code>
  </td>
  <td>Number of CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_revocable_total</code>
  </td>
  <td>Number of revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/cpus_revocable_used</code>
  </td>
  <td>Number of allocated revocable CPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_percent</code>
  </td>
  <td>Percentage of allocated disk space</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_used</code>
  </td>
  <td>Allocated disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_total</code>
  </td>
  <td>Disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_percent</code>
  </td>
  <td>Percentage of allocated GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_used</code>
  </td>
  <td>Number of allocated GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_total</code>
  </td>
  <td>Number of GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_revocable_total</code>
  </td>
  <td>Number of revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/gpus_revocable_used</code>
  </td>
  <td>Number of allocated revocable GPUs</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_percent</code>
  </td>
  <td>Percentage of allocated memory</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable disk space</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_revocable_total</code>
  </td>
  <td>Revocable disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/disk_revocable_used</code>
  </td>
  <td>Allocated revocable disk space in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_used</code>
  </td>
  <td>Allocated memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_total</code>
  </td>
  <td>Memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_revocable_percent</code>
  </td>
  <td>Percentage of allocated revocable memory</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_revocable_total</code>
  </td>
  <td>Revocable memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/mem_revocable_used</code>
  </td>
  <td>Allocated revocable memory in MB</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>volume_gid_manager/volume_gids_total</code>
  </td>
  <td>Number of gids configured for volume gid manager</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>volume_gid_manager/volume_gids_free</code>
  </td>
  <td>Number of free gids available for volume gid manager</td>
  <td>Gauge</td>
</tr>
</table>

#### Agent

The following metrics provide information about whether an agent is currently
registered with a master and for how long it has been running.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>slave/registered</code>
  </td>
  <td>Whether this agent is registered with a master</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/uptime_secs</code>
  </td>
  <td>Uptime in seconds</td>
  <td>Gauge</td>
</tr>
</table>

#### System

The following metrics provide information about the agent system.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>system/cpus_total</code>
  </td>
  <td>Number of CPUs available</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_15min</code>
  </td>
  <td>Load average for the past 15 minutes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_5min</code>
  </td>
  <td>Load average for the past 5 minutes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/load_1min</code>
  </td>
  <td>Load average for the past minute</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/mem_free_bytes</code>
  </td>
  <td>Free memory in bytes</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>system/mem_total_bytes</code>
  </td>
  <td>Total memory in bytes</td>
  <td>Gauge</td>
</tr>
</table>

#### Executors

The following metrics provide information about the executor instances running
on the agent.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>containerizer/mesos/container_destroy_errors</code>
  </td>
  <td>Number of containers destroyed due to launch errors</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>containerizer/fetcher/task_fetches_succeeded</code>
  </td>
  <td>Total number of times the Mesos fetcher successfully fetched all the URIs for a task.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>containerizer/fetcher/task_fetches_failed</code>
  </td>
  <td>Number of times the Mesos fetcher failed to fetch all the URIs for a task.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/container_launch_errors</code>
  </td>
  <td>Number of container launch errors</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/executors_preempted</code>
  </td>
  <td>Number of executors destroyed due to preemption</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/frameworks_active</code>
  </td>
  <td>Number of active frameworks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/executor_directory_max_allowed_age_secs</code>
  </td>
  <td>Maximum allowed age in seconds to delete executor directory</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/executors_registering</code>
  </td>
  <td>Number of executors registering</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/executors_running</code>
  </td>
  <td>Number of executors running</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/executors_terminated</code>
  </td>
  <td>Number of terminated executors</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/executors_terminating</code>
  </td>
  <td>Number of terminating executors</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/recovery_errors</code>
  </td>
  <td>Number of errors encountered during agent recovery</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/recovery_time_secs</code>
  </td>
  <td>Agent recovery time in seconds. This value is only available after agent
  recovery succeeded and remains constant for the life of the Mesos agent.</td>
  <td>Gauge</td>
</tr>
</table>

#### Tasks

The following metrics provide information about active and terminated tasks.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>slave/tasks_failed</code>
  </td>
  <td>Number of failed tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_finished</code>
  </td>
  <td>Number of finished tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_killed</code>
  </td>
  <td>Number of killed tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_lost</code>
  </td>
  <td>Number of lost tasks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_running</code>
  </td>
  <td>Number of running tasks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_staging</code>
  </td>
  <td>Number of staging tasks</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>slave/tasks_starting</code>
  </td>
  <td>Number of starting tasks</td>
  <td>Gauge</td>
</tr>
</table>

#### Messages

The following metrics provide information about messages between the agents and
the master it is registered with.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>slave/invalid_framework_messages</code>
  </td>
  <td>Number of invalid framework messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/invalid_status_updates</code>
  </td>
  <td>Number of invalid status updates</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/valid_framework_messages</code>
  </td>
  <td>Number of valid framework messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>slave/valid_status_updates</code>
  </td>
  <td>Number of valid status updates</td>
  <td>Counter</td>
</tr>
</table>

#### Containerizers

The following metrics provide information about both Mesos and Docker
containerizers.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms</code>
  </td>
  <td>Docker containerizer image pull latency in ms </td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/count</code>
  </td>
  <td>Number of Docker containerizer image pulls</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/max</code>
  </td>
  <td>Maximum Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/min</code>
  </td>
  <td>Minimum Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p50</code>
  </td>
  <td>Median Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p90</code>
  </td>
  <td>90th percentile Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p95</code>
  </td>
  <td>95th percentile Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p99</code>
  </td>
  <td>99th percentile Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p999</code>
  </td>
  <td>99.9th percentile Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/docker/image_pull_ms/p9999</code>
  </td>
  <td>99.99th percentile Docker containerizer image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/disk/project_ids_free</code>
  </td>
  <td>Number of free project IDs available to the XFS Disk isolator</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/disk/project_ids_total</code>
  </td>
  <td>Number of project IDs configured for the XFS Disk isolator</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms</code>
  </td>
  <td>Mesos containerizer docker image pull latency in ms </td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/count</code>
  </td>
  <td>Number of Mesos containerizer docker image pulls</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/max</code>
  </td>
  <td>Maximum Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/min</code>
  </td>
  <td>Minimum Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p50</code>
  </td>
  <td>Median Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p90</code>
  </td>
  <td>90th percentile Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p95</code>
  </td>
  <td>95th percentile Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p99</code>
  </td>
  <td>99th percentile Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p999</code>
  </td>
  <td>99.9th percentile Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>containerizer/mesos/provisioner/docker_store/image_pull_ms/p9999</code>
  </td>
  <td>99.99th percentile Mesos containerizer docker image pull latency in ms</td>
  <td>Gauge</td>
</tr>
</table>

#### Resource Providers

The following metrics provide information about ongoing and completed
[operations](operations.md) that apply to resources provided by a
[resource provider](resource-provider.md) with the given _type_ and _name_. In
the following metrics, the _operation_ placeholder refers to the name of a
particular operation type, which is described in the list of
[supported operation types](#supported-operation-types).

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/operations/<i>&lt;operation&gt;</i>/pending</code>
  </td>
  <td>Number of ongoing <i>operation</i>s</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/operations/<i>&lt;operation&gt;</i>/finished</code>
  </td>
  <td>Number of finished <i>operation</i>s</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/operations/<i>&lt;operation&gt;</i>/failed</code>
  </td>
  <td>Number of failed <i>operation</i>s</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/operations/<i>&lt;operation&gt;</i>/dropped</code>
  </td>
  <td>Number of dropped <i>operation</i>s</td>
  <td>Counter</td>
</tr>
</table>

##### Supported Operation Types

Since the supported operation types may vary among different resource providers,
the following is a comprehensive list of operation types and the corresponding
resource providers that support them. Note that the name column is for the
_operation_ placeholder in the above metrics.

<table class="table table-striped">
<thead>
<tr><th>Type</th><th>Name</th><th>Supported Resource Provider Types</th>
</thead>
<tr>
  <td><code><a href="reservation.md">RESERVE</a></code></td>
  <td><code>reserve</code></td>
  <td>All</td>
</tr>
<tr>
  <td><code><a href="reservation.md">UNRESERVE</a></code></td>
  <td><code>unreserve</code></td>
  <td>All</td>
</tr>
<tr>
  <td><code><a href="persistent-volume.md#-offer-operation-create-">CREATE</a></code></td>
  <td><code>create</code></td>
  <td><code>org.apache.mesos.rp.local.storage</code></td>
</tr>
<tr>
  <td><code><a href="persistent-volume.md#-offer-operation-destroy-">DESTROY</a></code></td>
  <td><code>destroy</code></td>
  <td><code>org.apache.mesos.rp.local.storage</code></td>
</tr>
<tr>
  <td><code><a href="csi.md#-create_disk-operation">CREATE_DISK</a></code></td>
  <td><code>create_disk</code></td>
  <td><code>org.apache.mesos.rp.local.storage</code></td>
</tr>
<tr>
  <td><code><a href="csi.md#-destroy_disk-operation">DESTROY_DISK</a></code></td>
  <td><code>destroy_disk</code></td>
  <td><code>org.apache.mesos.rp.local.storage</code></td>
</tr>
</table>

For example, cluster operators can monitor the number of successful
`CREATE_VOLUME` operations that are applied to the resource provider with type
`org.apache.mesos.rp.local.storage` and name `lvm` through the
`resource_providers/org.apache.mesos.rp.local.storage.lvm/operations/create_disk/finished`
metric.

#### CSI Plugins

Storage resource providers in Mesos are backed by
[CSI plugins](csi.md#standalone-containers-for-csi-plugins) running in
[standalone containers](standalone-container.md). To monitor the health of these
CSI plugins for a storage resource provider with _type_ and _name_, the
following metrics provide information about plugin terminations and ongoing and
completed CSI calls made to the plugin.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/csi_plugin/container_terminations</code>
  </td>
  <td>Number of terminated CSI plugin containers</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/csi_plugin/rpcs_pending</code>
  </td>
  <td>Number of ongoing CSI calls</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/csi_plugin/rpcs_finished</code>
  </td>
  <td>Number of successful CSI calls</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/csi_plugin/rpcs_failed</code>
  </td>
  <td>Number of failed CSI calls</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>resource_providers/<i>&lt;type&gt;</i>.<i>&lt;name&gt;</i>/csi_plugin/rpcs_cancelled</code>
  </td>
  <td>Number of cancelled CSI calls</td>
  <td>Counter</td>
</tr>
</table>
