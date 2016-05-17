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

## Framework Configuration

A framework can control whether its executors will be recovered by setting the `checkpoint` flag in its `FrameworkInfo` when registering with the master. Enabling this feature results in increased I/O overhead at each agent that runs tasks launched by the framework. By default, frameworks do **not** checkpoint their state.

## Agent Configuration

Three [configuration flags](configuration.md) control the recovery behavior of a Mesos agent:

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
> the executors and tasks running at a agent die when the agent dies
> and are not recovered.

A restarted agent should re-register with master within a timeout (75 seconds by default: see the `--max_agent_ping_timeouts` and `--agent_ping_timeout` [configuration flags](configuration.md)). If the agent takes longer than this timeout to re-register, the master shuts down the agent, which in turn will shutdown any live executors/tasks.  Therefore, it is highly recommended to automate the process of restarting a agent (e.g., using a process supervisor such as [monit](http://mmonit.com/monit/) or `systemd`).

## Known issues with `systemd` and POSIX isolation

There is a known issue when using `systemd` to launch the `mesos-agent` while also using only `posix` isolation mechanisms that prevents tasks from recovering. The problem is that the default [KillMode](http://www.freedesktop.org/software/systemd/man/systemd.kill.html) for systemd processes is `cgroup` and hence all child processes are killed when the agent stops. Explicitly setting `KillMode` to `process` allows the executors to survive and reconnect.

The following excerpt of a `systemd` unit configuration file shows how to set the flag:

```
[Service]
ExecStart=/usr/bin/mesos-agent
KillMode=process
```


> NOTE: There are also known issues with using `systemd` and raw `cgroups` based isolation, for now the suggested non-Posix isolation mechanism is to use Docker containerization.
