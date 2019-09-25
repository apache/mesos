---
title: Apache Mesos - Cgroups 'cpu' Subsystem Support in Mesos Containerizer
layout: documentation
---

# Cgroups 'cpu' and 'cpuacct' Subsystems Support in Mesos Containerizer

The `cgroups/cpu` isolator allows operators to provide CPU isolation
and CPU usage accounting for containers within a Mesos cluster. To
enable the `cgroups/cpu` isolator, append `cgroups/cpu` to the
`--isolation` flag when starting the Mesos agent.

## Subsystems

The isolator enables `cpu` and `cpuacct` subsystems for Linux cgroups
and assigns `cpu` and `cpuacct` cgroups to each container launched by
Mesos Containerizer.

Cgroups `cpu` subsystem provides 2 mechanisms of limiting the amount
of CPU time used by cgroups: [CFS shares](https://github.com/torvalds/linux/blob/master/Documentation/scheduler/sched-design-CFS.rst)
and [CFS bandwidth](https://github.com/torvalds/linux/blob/master/Documentation/scheduler/sched-bwc.rst)
control. The first one can guarantee some minimum number of CPU
"shares" to a cgroup when the system is under heavy load. It, however,
does not limit the amount of CPU time available to a cgroup when the
system is not busy. This mechanism is always enabled when you turn on
this isolator.

The second mechanism introduces a way to define the upper limit of CPU
time used by a cgroup within every scheduling period. Additionally it
exports bandwidth statistics in `cpu.stat` file. CFS bandwidth
mechanism can be enabled with `--cgroups_enable_cfs` agent flag.

Cgroups `cpuacct` subsystem provides accounting for CPU usage by
cgroup's tasks. Currently it only provides statistics in
`cpuacct.stat` that show time spent by tasks of the cgroup in user
mode and kernel mode respectively

Additionally the isolator provides accounting for the number of
processes and threads inside a container based on the information from
`cgroups.procs` and `tasks` files.

## Effects on application when using CFS Bandwidth Limiting

CPU usage is monitored during a scheduling period, which is currently
set to 100ms. A task can use its requested CPU time before that period
ends. For example, if a task is allocated 1 CPUs, it can use 2 CPU
cores for 50ms thus consuming 100ms of CPU time, and be throttled for
the remaining 50ms. Applications that are latency sensitive may suffer
from effectively being stalled for 50ms.

## Statistics

This isolator exports several valuable per-container CPU usage metrics
that are available from agent's `monitor/statistics`
[endpoint](../endpoints/slave/monitor/statistics.md).

  * `processes` - number of processes in the cgroup;
  * `threads` - number of threads in the cgroup;
  * `cpus_user_time_secs` - time spent by tasks of the cgroup in user
    mode;
  * `cpus_system_time_secs` - time spent by tasks of the cgroup in
    kernel mode;
  * `cpus_nr_periods` - number of enforcement intervals that have
    elapsed;
  * `cpus_nr_throttled` - number of times the cgroup has been
    throttled;
  * `cpus_throttled_time` - total time for which tasks in the cgroup
    have been throttled.

## Configuration

<table class="table table-striped">
  <thead>
    <tr>
      <th>Flag</th>
      <th>Explanation</th>
    </tr>
  </thead>
  <tr>
    <td>
      --[no]-cgroups_cpu_enable_pids_and_tids_count
    </td>
    <td>
Cgroups feature flag to enable counting of processes and threads
inside a container. (default: false)
  </td>
  </tr>
  <tr>
    <td>
      --[no]-cgroups_enable_cfs
    </td>
    <td>
Cgroups feature flag to enable hard limits on CPU resources via the
CFS bandwidth limiting subfeature. (default: false)
    </td>
  </tr>
  <tr>
    <td>
      --[no-]revocable_cpu_low_priority
    </td>
    <td>
Run containers with revocable CPU at a lower priority than normal
containers (non-revocable CPU). (default: true)
    </td>
  </tr>
</table>
