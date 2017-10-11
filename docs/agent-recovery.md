---
title: Apache Mesos - Agent Recovery
layout: documentation
---

# Agent Recovery

If the `mesos-agent` process on a host exits (perhaps due to a Mesos bug or
because the operator kills the process while [upgrading Mesos](upgrades.md)),
any executors/tasks that were being managed by the `mesos-agent` process will
continue to run. When `mesos-agent` is restarted, the operator can control how
those old executors/tasks are handled:

 1. By default, all the executors/tasks that were being managed by the old
    `mesos-agent` process are killed.
 2. If a framework enabled _checkpointing_ when it registered with the master,
    any executors belonging to that framework can reconnect to the new
    `mesos-agent` process and continue running uninterrupted.

Hence, enabling framework checkpointing enables tasks to tolerate Mesos agent
upgrades and unexpected `mesos-agent` crashes without experiencing any
downtime.

Agent recovery works by having the agent _checkpoint_ information (e.g., Task
Info, Executor Info, Status Updates) about the tasks and executors it is
managing to local disk. If a framework enables checkpointing, any subsequent
agent restarts will recover the checkpointed information and reconnect with any
executors that are still running.

Note that if the operating system on the agent is rebooted, all executors and
tasks running on the host are killed and are not automatically restarted when
the host comes back up.

However the agent is allowed to recover its agent ID post a host reboot.
In case the agent's recovery runs into agent info mismatch which may happen due to resource change associated with reboot, it'll fall back to recovering as a new agent (existing behavior).
In other cases such as checkpointed resources (e.g. persistent volumes) being incompatible with the agent's resources the recovery will still fail (existing behavior).

## Framework Configuration

A framework can control whether its executors will be recovered by setting the `checkpoint` flag in its `FrameworkInfo` when registering with the master. Enabling this feature results in increased I/O overhead at each agent that runs tasks launched by the framework. By default, frameworks do **not** checkpoint their state.

## Agent Configuration

Three [configuration flags](configuration/agent.md) control the recovery behavior of a Mesos agent:

* `strict`: Whether to do agent recovery in strict mode [Default: true].
    - If strict=true, all recovery errors are considered fatal.
    - If strict=false, any errors (e.g., corruption in checkpointed data) during recovery are
      ignored and as much state as possible is recovered.

* `recover`: Whether to recover status updates and reconnect with old executors [Default: reconnect].
    - If recover=reconnect, reconnect with any old live executors, provided the executor's framework enabled checkpointing.
    - If recover=cleanup, kill any old live executors and exit. Use this option when doing an incompatible agent or executor upgrade!
    > NOTE: If no checkpointing information exists, no recovery is performed
    > and the agent registers with the master as a new agent.

* `recovery_timeout`: Amount of time allotted for the agent to recover [Default: 15 mins].
    - If the agent takes longer than `recovery_timeout` to recover, any executors that are waiting to
      reconnect to the agent will self-terminate.

> NOTE: If none of the frameworks have enabled checkpointing,
> the executors and tasks running at an agent die when the agent dies
> and are not recovered.

A restarted agent should re-register with master within a timeout (75 seconds by default: see the `--max_agent_ping_timeouts` and `--agent_ping_timeout` [configuration flags](configuration.md)). If the agent takes longer than this timeout to re-register, the master shuts down the agent, which in turn will shutdown any live executors/tasks. Therefore, it is highly recommended to automate the process of restarting an agent (e.g., using a process supervisor such as [monit](http://mmonit.com/monit/) or `systemd`).

## Known issues with `systemd` and process lifetime

There is a known issue when using `systemd` to launch the `mesos-agent`. A description of the problem can be found in [MESOS-3425](https://issues.apache.org/jira/browse/MESOS-3425) and all relevant work can be tracked in the epic [MESOS-3007](https://issues.apache.org/jira/browse/MESOS-3007).
This problem was fixed in Mesos `0.25.0` for the mesos containerizer when cgroups isolation is enabled. Further fixes for the posix isolators and docker containerizer are available in `0.25.1`, `0.26.1`, `0.27.1`, and `0.28.0`.

It is recommended that you use the default [KillMode](http://www.freedesktop.org/software/systemd/man/systemd.kill.html) for systemd processes, which is `control-group`, which kills all child processes when the agent stops. This ensures that "side-car" processes such as the `fetcher` and `perf` are terminated alongside the agent.
The systemd patches for Mesos explicitly move executors and their children into a separate systemd slice, dissociating their lifetime from the agent. This ensures the executors survive agent restarts.

The following excerpt of a `systemd` unit configuration file shows how to set the flag explicitly:

```
[Service]
ExecStart=/usr/bin/mesos-agent
KillMode=control-cgroup
```
