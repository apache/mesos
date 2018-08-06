---
title: Apache Mesos - Agent Recovery
layout: documentation
---

# Agent Recovery

If the `mesos-agent` process on a host exits (perhaps due to a Mesos bug or
because the operator kills the process while [upgrading Mesos](upgrades.md)),
any executors/tasks that were being managed by the `mesos-agent` process will
continue to run.

By default, all the executors/tasks that were being managed by the old
`mesos-agent` process are expected to gracefully exit on their own, and
will be shut down after the agent restarted if they did not.

However, if a framework enabled  _checkpointing_ when it registered with the
master, any executors belonging to that framework can reconnect to the new
`mesos-agent` process and continue running uninterrupted. Hence, enabling
framework checkpointing allows tasks to tolerate Mesos agent upgrades and
unexpected `mesos-agent` crashes without experiencing any downtime.

Agent recovery works by having the agent checkpoint information about its own
state and about the tasks and executors it is managing to local disk, for
example the `SlaveInfo`, `FrameworkInfo` and `ExecutorInfo` messages or the
unacknowledged status updates of running tasks.

When the agent restarts, it will verify that its current configuration, set
from the environment variables and command-line flags, is compatible with the
checkpointed information and will refuse to restart if not.

A special case occurs when the agent detects that its host system was rebooted
since the last run of the agent: The agent will try to recover its previous ID
as usual, but if that fails it will actually erase the information of the
previous run and will register with the master as a new agent.

Note that executors and tasks that exited between agent shutdown and restart
are not automatically restarted during agent recovery.

## Framework Configuration

A framework can control whether its executors will be recovered by setting
the `checkpoint` flag in its `FrameworkInfo` when registering with the master.
Enabling this feature results in increased I/O overhead at each agent that runs
tasks launched by the framework. By default, frameworks do **not** checkpoint
their state.

## Agent Configuration

Four [configuration flags](configuration/agent.md) control the recovery
behavior of a Mesos agent:

* `strict`: Whether to do agent recovery in strict mode [Default: true].
    - If strict=true, all recovery errors are considered fatal.
    - If strict=false, any errors (e.g., corruption in checkpointed data) during
      recovery are ignored and as much state as possible is recovered.

* `reconfiguration_policy`: Which kind of configuration changes are accepted
  when trying to recover [Default: equal].
    - If reconfiguration_policy=equal, no configuration changes are accepted.
    - If reconfiguration_policy=additive, the agent will allow the new
      configuration to contain additional attributes, increased resourced or an
      additional fault domain. For a more detailed description, see
      [this](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob;f=src/slave/compatibility.hpp;h=78b421a01abe5d2178c93832577577a7ba282b38;hb=HEAD#l37).

* `recover`: Whether to recover status updates and reconnect with old
  executors [Default: reconnect]
    - If recover=reconnect, reconnect with any old live executors, provided
      the executor's framework enabled checkpointing.
    - If recover=cleanup, kill any old live executors and exit. Use this
      option when doing an incompatible agent or executor upgrade!
      **NOTE:** If no checkpointing information exists, no recovery is performed
      and the agent registers with the master as a new agent.

* `recovery_timeout`: Amount of time allotted for the agent to
  recover [Default: 15 mins].
    - If the agent takes longer than `recovery_timeout` to recover, any
      executors that are waiting to reconnect to the agent will self-terminate.
      **NOTE:** If none of the frameworks have enabled checkpointing, the
      executors and tasks running at an agent die when the agent dies and are
      not recovered.

A restarted agent should reregister with master within a timeout (75 seconds
by default: see the `--max_agent_ping_timeouts` and `--agent_ping_timeout`
[configuration flags](configuration.md)). If the agent takes longer than this
timeout to reregister, the master shuts down the agent, which in turn will
shutdown any live executors/tasks.

Therefore, it is highly recommended to automate the process of restarting an
agent, e.g. using a process supervisor such as [monit](http://mmonit.com/monit/)
or `systemd`.

## Known issues with `systemd` and process lifetime

There is a known issue when using `systemd` to launch the `mesos-agent`. A
description of the problem can be found in [MESOS-3425](https://issues.apache.org/jira/browse/MESOS-3425)
and all relevant work can be tracked in the epic [MESOS-3007](https://issues.apache.org/jira/browse/MESOS-3007).

This problem was fixed in Mesos `0.25.0` for the mesos containerizer when
cgroups isolation is enabled. Further fixes for the posix isolators and docker
containerizer are available in `0.25.1`, `0.26.1`, `0.27.1`, and `0.28.0`.

It is recommended that you use the default [KillMode](http://www.freedesktop.org/software/systemd/man/systemd.kill.html)
for systemd processes, which is `control-group`, which kills all child processes
when the agent stops. This ensures that "side-car" processes such as the
`fetcher` and `perf` are terminated alongside the agent.
The systemd patches for Mesos explicitly move executors and their children into
a separate systemd slice, dissociating their lifetime from the agent. This
ensures the executors survive agent restarts.

The following excerpt of a `systemd` unit configuration file shows how to set
the flag explicitly:

```
[Service]
ExecStart=/usr/bin/mesos-agent
KillMode=control-cgroup
```
