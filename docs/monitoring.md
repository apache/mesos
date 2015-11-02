---
layout: documentation
---


# Mesos Observability Metrics

This document describes the observability metrics provided by Mesos master and
slave nodes. This document also provides some initial guidance on which metrics
you should monitor to detect abnormal situations in your cluster.


## Overview

Mesos master and slave nodes report a set of statistics and metrics that enable
you to  monitor resource usage and detect abnormal situations early. The
information reported by Mesos includes details about available resources, used
resources, registered frameworks, active slaves, and task state. You can use
this information to create automated alerts and to plot different metrics over
time inside a monitoring dashboard.


## Metric Types

Mesos provides two different kinds of metrics: counters and gauges.

**Counters** keep track of discrete events and are monotonically increasing. The
value of a metric of this type is always a natural number. Examples include the
number of failed tasks and the number of slave registrations. For some metrics
of this type, the rate of change is often more useful than the value itself.

**Gauges** represent an instantaneous sample of some magnitude. Examples include
the amount of used memory in the cluster and the number of connected slaves. For
some metrics of this type, it is often useful to determine whether the value is
above or below a threshold for a sustained period of time.

The tables in this document indicate the type of each available metric.


## Master Nodes

Metrics from the master node are available at the following URL:

    http://<mesos-master-ip>:5050/metrics/snapshot

The response is a JSON object that contains metrics names and values as
key-value pairs.

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

#### Slaves

The following metrics provide information about slave events, slave counts, and
slave states. A low number of active slaves may indicate that slaves are
unhealthy or that they are not able to connect to the elected master.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>master/slave_registrations</code>
  </td>
  <td>Number of slaves that were able to cleanly re-join the cluster and
      connect back to the master after the master is disconnected.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals</code>
  </td>
  <td>Number of slave removed for various reasons, including maintenance</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_reregistrations</code>
  </td>
  <td>Number of slave re-registrations</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_shutdowns_scheduled</code>
  </td>
  <td>Number of slaves which have failed their health check and are scheduled
      to be removed. They will not be immediately removed due to the Slave
      Removal Rate-Limit, but <code>master/slave_shutdowns_completed</code>
      will start increasing as they do get removed.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_shutdowns_canceled</code>
  </td>
  <td>Number of cancelled slave shutdowns. This happens when the slave removal
      rate limit allows for a slave to reconnect and send a <code>PONG</code>
      to the master before being removed.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_shutdowns_completed</code>
  </td>
  <td>Number of slaves that failed their health check. These are slaves which
      were not heard from despite the slave-removal rate limit, and have been
      removed from the master's slave registry.</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slaves_active</code>
  </td>
  <td>Number of active slaves</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_connected</code>
  </td>
  <td>Number of connected slaves</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_disconnected</code>
  </td>
  <td>Number of disconnected slaves</td>
  <td>Gauge</td>
</tr>
<tr>
  <td>
  <code>master/slaves_inactive</code>
  </td>
  <td>Number of inactive slaves</td>
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
</table>

#### Messages

The following metrics provide information about messages between the master and
the slaves and between the framework and the executors. A high rate of dropped
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
  <td>Number of slave registration messages</td>
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
  <td>Number of slave re-registration messages</td>
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
  <td>Number of slave unregistration messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/messages_update_slave</code>
  </td>
  <td>Number of update slave messages</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/recovery_slave_removals</code>
  </td>
  <td>Number of slaves not re-registered during master failover</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_registered</code>
  </td>
  <td>Number of slaves removed when new slaves registered at the same address</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_unhealthy</code>
  </td>
  <td>Number of slaves failed due to failed health checks</td>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/slave_removals/reason_unregistered</code>
  </td>
  <td>Number of slaves unregistered</td>
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
  <td>Number of tasks lost due to invalid offers</code>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/task_lost/source_master/reason_slave_removed</code>
  </td>
  <td>Number of tasks lost due to slave removal</code>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/task_lost/source_slave/reason_executor_terminated</code>
  </td>
  <td>Number of tasks lost due to executor termination</code>
  <td>Counter</td>
</tr>
<tr>
  <td>
  <code>master/valid_executor_to_framework_messages</code>
  </td>
  <td>Number of valid executor to framework messages</code>
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
</table>

#### Registrar

The following metrics provide information about read and write latency to the
slave registrar.

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

Slaves are having trouble connecting to the master.

#### master/cpus_percent > 0.9 for sustained periods of time

Cluster CPU utilization is close to capacity.

#### master/mem_percent > 0.9 for sustained periods of time

Cluster memory utilization is close to capacity.

#### master/elected is 0 for sustained periods of time

No master is currently elected.




## Slave Nodes

Metrics from each slave node are available at the following URL:

    http://<mesos-slave>:5051/metrics/snapshot

The response is a JSON object that contains metrics names and values as key-
value pairs.


### Observability Metrics

This section lists all available metrics from Mesos slave nodes grouped by
category.

#### Resources

The following metrics provide information about the total resources available in
the slave and their current usage.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
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
</table>

#### Slave

The following metrics provide information about whether a slave is currently
registered with a master and for how long it has been running.

<table class="table table-striped">
<thead>
<tr><th>Metric</th><th>Description</th><th>Type</th>
</thead>
<tr>
  <td>
  <code>slave/registered</code>
  </td>
  <td>Whether this slave is registered with a master</td>
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

The following metrics provide information about the slave system.

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
on the slave.

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
  <td>Number of errors encountered during slave recovery</td>
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

The following metrics provide information about messages between the slaves and
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
