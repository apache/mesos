---
title: Apache Mesos - POSIX Resource Limits Support in Mesos Containerizer
layout: documentation
---

# POSIX Resource Limits Support in Mesos Containerizer

This document describes the `posix/rlimits` isolator. The isolator adds support
for setting POSIX resource limits (rlimits) for containers launched using the
[Mesos containerizer](../mesos-containerizer.md).

To enable the POSIX Resource Limits support, append `posix/rlimits` to
the `--isolation` flag when starting the agent.

## POSIX Resource Limits

POSIX rlimits can be used control the resources a process can consume. Resource
limits are typically set at boot time and inherited when a child process is
forked from a parent process; resource limits can also be modified via
`setrlimit(2)`. In many interactive shells, resource limits can be inspected or
modified with the `ulimit` shell built-in.

A POSIX resource limit consist of a _soft_ and a _hard_ limit. The soft limit
specifies the effective resource limit for the current and forked process, while
the hard limit gives the value up to which processes may increase their
effective limit; increasing the hard limit is a privileged action. It is
required that the soft limit is less than or equal to the hard limit.
System administrators can use a hard resource limit to define the maximum amount
of resources that can be consumed by a user; users can employ soft resource
limits to ensure that one of their tasks only consumes a limited amount of the
global hard resource limit.


## Setting POSIX Resource Limits for Tasks

This isolator permits setting per-task resource limits. This isolator interprets
rlimits specified as part of a task's `ContainerInfo` for the Mesos
containerizer, e.g.,

~~~{.json}
{
  "container": {
    "type": "MESOS",
    "rlimit_info": {
      "rlimits": [
        {
          "type": "RLMT_CORE"
        },
        {
          "type": "RLMT_STACK",
          "soft": 8192,
          "hard": 32768
        }
      ]
    }
  }
}
~~~

To enable interpretation of rlimits, agents need to
be started with `posix/rlimits` in its `--isolation` flag, e.g.,

~~~{.console}
mesos-agent --master=<master ip> --ip=<agent ip>
  --work_dir=/var/lib/mesos
  --isolation=posix/rlimits[,other isolation flags]
~~~

To set a hard limit for a task larger than the current value of the hard limit,
the agent process needs to be under a privileged user (with the
`CAP_SYS_RESOURCE` capability), typically `root`.

POSIX currently defines a base set of resources, see
[the documentation](http://pubs.opengroup.org/onlinepubs/009695399/functions/getrlimit.html);
Linux defines additional resource limits, see e.g., the documentation of
`setrlimit(2)`.

<table class="table table-striped">
  <thead>
    <tr>
      <th>Resource</th>
      <th>Comment</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>RLIMIT_CORE</code></td>
      <td><em>POSIX</em>: This is the maximum size of a core file, in bytes, that may be created by a process.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_CPU</code></td>
      <td><em>POSIX</em>: This is the maximum amount of CPU time, in seconds, used by a process.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_DATA</code></td>
      <td><em>POSIX</em>: This is the maximum size of a process' data segment, in bytes.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_FSIZE</code></td>
      <td><em>POSIX</em>: This is the maximum size of a file, in bytes, that may be created by a process.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_NOFILE</code></td>
      <td><em>POSIX</em>: This is a number one greater than the maximum value that the system may assign to a newly-created descriptor.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_STACK</code></td>
      <td><em>POSIX</em>: This is the maximum size of the initial thread's stack, in bytes.</td>
    </tr>
    <tr>
      <td><code>RLIMIT_AS</code></td>
      <td><em>POSIX</em>: This is the maximum size of a process' total available memory, in bytes.</td>
    </tr>
    <tr>
      <td><code>RLMT_LOCKS</code></td>
      <td><em>Linux</em>: (Early Linux 2.4 only) A limit on the combined number of <code>flock(2)</code> locks and <code>fcntl(2)</code> leases that this process may establish.</td>
    </tr>
    <tr>
      <td><code>RLMT_MEMLOCK</code></td>
      <td><em>Linux</em>: The maximum number of bytes of memory that may be locked into RAM.</td>
    </tr>
    <tr>
      <td><code>RLMT_MSGQUEUE</code></td>
      <td><em>Linux</em>: Specifies the limit on the number of bytes that can be allocated for POSIX message queues for the real user ID of the calling process.</td>
    </tr>
    <tr>
      <td><code>RLMT_NICE</code></td>
      <td><em>Linux</em>: (Since Linux 2.6.12) Specifies a ceiling to which the process's nice value can be raised using <code>setpriority(2)</code> or <code>nice(2)</code>.</td>
    </tr>
    <tr>
      <td><code>RLMT_NPROC</code></td>
      <td><em>Linux</em>: The maximum number of processes (or, more precisely on Linux, threads) that can be created for the real user ID of the calling process.</td>
    </tr>
    <tr>
      <td><code>RLMT_RSS</code></td>
      <td><em>Linux</em>: Specifies the limit (in pages) of the process's resident set (the number of virtual pages resident in RAM).</td>
    </tr>
    <tr>
      <td><code>RLMT_RTPRIO</code></td>
      <td><em>Linux</em>: (Since Linux 2.6.12) Specifies a ceiling on the real-time priority that may be set for this process using sched_setscheduler(2) and sched_setparam(2).</td>
    </tr>
    <tr>
      <td><code>RLMT_RTTIME</code></td>
      <td><em>Linux</em>: (Since Linux 2.6.25) Specifies a limit (in microseconds) on the amount of CPU time that a process scheduled under a real-time scheduling policy may consume without making a blocking system call.</td>
    </tr>
    <tr>
      <td><code>RLMT_SIGPENDING</code></td>
      <td><em>Linux</em>: (Since Linux 2.6.8) Specifies the limit on the number of signals that may be queued for the real user ID of the calling process.</td>
    </tr>
  </tbody>
</table>

Mesos maps these resource types onto `RLimit` types, where by convention the
prefix `RLMT_` is used in place of `RLIMIT_` above. Not all limits types are
supported on all platforms.

We require either both the soft and hard `RLimit` value, or none to be
set; the latter case is interpreted as the absence of an explicit limit.
