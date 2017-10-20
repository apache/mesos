---
title: Apache Mesos - Windows Job Object Support in Mesos Containerizer
layout: documentation
---

# Windows Job Object Support in Mesos Containerizer

The `windows/cpu` isolator allows operators to provide CPU limitations and CPU
usage accounting for containers within a Mesos cluster. To enable the
`windows/cpu` isolator, append `windows/cpu` to the `--isolation` flag when
starting the Mesos agent.

The `windows/mem` isolator allows operators to provide memory limitations and
memory usage accounting for containers within a Mesos cluster. To enable the
`windows/mem` isolator, append `windows/mem` to the `--isolation` flag when
starting the Mesos agent.

Both of these isolators apply to commands launched by the Mesos Containerizer,
which are grouped into a Windows [Job Object](https://msdn.microsoft.com/en-us/library/windows/desktop/ms684161(v=vs.85).aspx).
They do not apply to Windows Containers or Docker Containers.

## CPU limits

The `windows/cpu` isolator sets a hard cap on the CPU usage, enforced by the
operating system. This uses [Job Object CPU Rate Control](https://msdn.microsoft.com/en-us/library/windows/desktop/hh448384(v=vs.85).aspx):

> Sets the maximum portion of processor cycles that the threads in a job object
> can use during each scheduling interval. After the job reaches this limit for
> a scheduling interval, no threads associated with the job can run until the
> next scheduling interval.

NOTE: This *does not* support bursting. Unlike the similarly named `cgroups/cpu`
isolator, this is a **hard cap**, not a soft cap.

### Statistics

This isolator exports several valuable per-container CPU usage metrics
that are available from agent's `monitor/statistics`
[endpoint](../endpoints/slave/monitor/statistics.md).

* `processes` - number of processes in the job object;
* `cpus_user_time_secs` - time spent by tasks of the job object in user mode;
* `cpus_system_time_secs` - time spent by tasks of the job object in kernel
  mode.

## Memory limits

The `windows/memory` isolator sets a hard cap on the virtual memory usage, enforced by the
operating system. This uses [Job Object Limit Job Memory](https://msdn.microsoft.com/en-us/library/windows/desktop/ms684156(v=vs.85).aspx):

> Sets the limit for the virtual memory that can be committed for the job. When
> a process attempts to commit memory that would exceed the job-wide limit, it
> fails.

### Statistics

This isolator exports valuable per-container memory usage metrics
that are available from agent's `monitor/statistics`
[endpoint](../endpoints/slave/monitor/statistics.md).

* `mem_total_bytes` - sum of the current working set size for all processes in
  the job object. This does not include memory that is swapped out.
