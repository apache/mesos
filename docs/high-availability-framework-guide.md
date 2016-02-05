---
layout: documentation
---

# Designing Highly Available Mesos Frameworks

A Mesos framework manages tasks. For a Mesos framework to be highly available,
it must continue to manage tasks correctly in the presence of a variety of
failure scenarios. The most common failure conditions that framework authors
should consider include:

* The Mesos master that a framework scheduler is connected to might fail, for
  example by crashing or by losing network connectivity. If the master has been
  configured to use [high-availability mode](high-availability.md), this will
  result in promoting another Mesos master replica to become the current
  leader. In this situation, the scheduler should re-register with the new
  master and ensure that task state is consistent.

* The host where a framework scheduler is running might fail. To ensure that the
  framework remains available and can continue to schedule new tasks, framework
  authors should ensure that multiple copies of the scheduler run on different
  nodes, and that a backup copy is promoted to become the new leader when the
  previous leader fails. Mesos itself does not dictate how framework authors
  should handle this situation, although we provide some suggestions below. It
  can be useful to deploy multiple copies of your framework scheduler using
  a long-running task scheduler such as Apache Aurora or Marathon.

* The host where a task is running might fail. Alternatively, the node itself
  might not have failed but the Mesos agent on the node might be unable to
  communicate with the Mesos master, e.g., due to a network partition.

Note that more than one of these failures might occur simultaneously.

## Mesos Architecture

Before discussing the specific failure scenarios outlined above, it is worth
highlighting some aspects of how Mesos is designed that influence high
availability:

* Mesos provides unreliable messaging between components by default: messages
  are delivered "at-most-once" (they might be dropped). Framework authors should
  expect that messages they send might not be received and be prepared to take
  appropriate corrective action. To detect that a message might be lost,
  frameworks typically use timeouts. For example, if a framework attempts to
  launch a task, that message might not be received by the Mesos master (e.g.,
  due to a transient network failure). To address this, the framework scheduler
  should set a timeout after attempting to launch a new task. If the scheduler
  hasn't seen a status update for the new task before the timeout fires, it
  should take corrective action---for example, by performing [task state reconciliation](reconciliation.md),
  and then launching a new copy of the task if necessary.

  * In general, distributed systems cannot distinguish between "lost" messages
    and messages that are merely delayed. In the example above, the scheduler
    might see a status update for the first task launch attempt immediately
    _after_ its timeout has fired and it has already begun taking corrective
    action. Scheduler authors should be aware of this possibility and program
    accordingly.

  * Mesos actually provides ordered (but unreliable) message delivery between
    any pair of processes: for example, if a framework sends messages M1 and M2
    to the master, the master might receive no messages, just M1, just M2, or M1
    followed by M2 -- it will _not_ receive M2 followed by M1.

  * As a convenience for framework authors, Mesos provides reliable delivery of
    task status updates. The agent persists task status updates to disk and then
    forwards them to the master. The master sends status updates to the
    appropriate framework scheduler. When a scheduler acknowledges a status
    update, the master forwards the acknowledgment back to the agent, which
    allows the stored status update to be garbage collected. If the agent does
    not receive an acknowledgment for a task status update within a certain
    amount of time, it will repeatedly resend the status update to the master,
    which will again forward the update to the scheduler. Hence, task status
    updates will be delivered "at least once", assuming that the agent and the
    scheduler both remain available. To handle the fact that task status updates
    might be delivered more than once, it can be helpful to make the framework
    logic that processes them [idempotent](https://en.wikipedia.org/wiki/Idempotence).

* The Mesos master stores information about the active tasks and registered
  frameworks _in memory_: it does not persist it to disk or attempt to ensure
  that this information is preserved after a master failover. This helps the
  Mesos master scale to large clusters with many tasks and frameworks. A
  downside of this design is that after a failure, more work is required to
  recover the lost in-memory master state.

* If all the Mesos masters are unavailable (e.g., crashed or unreachable), the
  cluster should continue to operate: existing Mesos agents and user tasks should
  continue running. However, new tasks cannot be scheduled, and frameworks will
  not receive resource offers or status updates about previously launched tasks.

* Mesos does not dictate how frameworks should be implemented and does not try
  to assume responsibility for how frameworks should deal with failures.
  Instead, Mesos tries to provide framework developers with the tools they need
  to implement this behavior themselves. Different frameworks might choose to
  handle failures differently, depending on their exact requirements.

## Recommendations for Highly Available Frameworks

Highly available framework designs typically follow a few common patterns:

1. To tolerate scheduler failures, frameworks run multiple scheduler instances
   (three instances is typical). At any given time, only one of these scheduler
   instances is the _leader_: this instance is connected to the Mesos master,
   receives resource offers and task status updates, and launches new tasks. The
   other scheduler replicas are _followers_: they are used only when the leader
   fails, in which case one of the followers is chosen to become the new leader.

2. Schedulers need a mechanism to decide when the current scheduler leader has
   failed and to elect a new leader. This is typically accomplished using a
   coordination service like [Apache ZooKeeper](https://zookeeper.apache.org/)
   or [etcd](https://github.com/coreos/etcd). Consult the documentation of the
   coordination system you are using for more information on how to correctly
   implement leader election.

3. After electing a new leading scheduler, the new leader should reconnect to
   the Mesos master. When registering with the master, the framework should set
   the `id` field in its `FrameworkInfo` to the ID that was assigned to the
   failed scheduler instance. This ensures that the master will recognize that
   the connection does not start a new session, but rather continues (and
   replaces) the session used by the failed scheduler instance.

    >NOTE: When the old scheduler leader disconnects from the master, by default
    the master will immediately kill all the tasks and executors associated with
    the failed framework. For a typical production framework, this default
    behavior is very undesirable! To avoid this, highly available frameworks
    should set the `failover_timeout` field in their `FrameworkInfo` to a
    generous value. To avoid accidental destruction of tasks in production
    environments, many frameworks use a `failover_timeout` of 1 week or more.

4. After connecting to the Mesos master, the new leading scheduler should ensure
   that its local state is consistent with the current state of the cluster. For
   example, suppose that the previous leading scheduler attempted to launch a
   new task and then immediately failed. The task might have launched
   successfully, at which point the newly elected leader will begin to receive
   status updates about it. To handle this situation, frameworks typically use a
   strongly consistent distributed data store to record information about active
   and pending tasks. In fact, the same coordination service that is used for
   leader election (such as ZooKeeper or etcd) can often be used for this
   purpose. Some Mesos frameworks (such as Apache Aurora) use the Mesos
   [replicated log](replicated-log-internals.md) for this purpose.

   * The data store should be used to record the actions that the scheduler
     _intends_ to take, before it takes them. For example, if a scheduler
     decides to launch a new task, it _first_ writes this intent to its data
     store. Then it sends a "launch task" message to the Mesos master. If this
     instance of the scheduler fails and a new scheduler is promoted to become
     the leader, the new leader can consult the data store to find _all possible
     tasks_ that might be running on the cluster. This is an instance of the
     [write-ahead logging](https://en.wikipedia.org/wiki/Write-ahead_logging)
     pattern often employed by database systems and filesystems to improve
     reliability. Two aspects of this design are worth emphasizing.

     1. The scheduler must persist its intent _before_ launching the task: if
        the task is launched first and then the scheduler fails before it can
        write to the data store, the new leading scheduler won't know about the
        new task. If this occurs, the new scheduler instance will begin
        receiving task status updates for a task that it has no knowledge of;
        there is often not a good way to recover from this situation.

     2. Second, the scheduler should ensure that its intent has been durably
        recorded in the data store before continuing to launch the task (for
        example, it should wait for a quorum of replicas in the data store to
        have acknowledged receipt of the write operation). For more details on
        how to do this, consult the documentation for the data store you are
        using.

## The Life Cycle of a Task

A Mesos task transitions through a sequence of states. The authoritative "source
of truth" for the current state of a task is the agent on which the task is
running. A framework scheduler learns about the current state of a task by
communicating with the Mesos master---specifically, by listening for task status
updates and by performing task state reconciliation.

Frameworks can represent the state of a task using a state machine, with one
initial state and several possible terminal states:

* A task begins in the `TASK_STAGING` state. A task is in this state when the
  master has received the framework's request to launch the task but the task
  has not yet started to run. In this state, the task's dependencies are
  fetched---for example, using the [Mesos fetcher cache](fetcher.md).

* The `TASK_STARTING` state is optional and intended primarily for use by
  custom executors. It can be used to describe the fact that a custom executor
  has learned about the task (and maybe started fetching its dependencies) but has
  not yet started to run it.

* A task transitions to the `TASK_RUNNING` state after it starts running
  successfully (if the task fails to start, it transitions to one of the
  terminal states listed below).

  * If a framework attempts to launch a task but does not receive a status
    update for it within a timeout, the framework should perform
    [reconciliation](reconciliation.md). That is, it should ask the master for
    the current state of the task. The master will reply with `TASK_LOST` for
    unknown tasks. The framework can then use this to distinguish between tasks
    that are slow to launch and tasks that the master has never heard about
    (e.g., because the task launch message was dropped).

    * Note that the correctness of this technique depends on the fact that
      messaging between the scheduler and the master is ordered.

* There are several terminal states:

  * `TASK_FINISHED` is used when a task completes successfully.
  * `TASK_FAILED` indicates that a task aborted with an error.
  * `TASK_KILLED` indicates that a task was killed by the executor.
  * `TASK_LOST` indicates that the task was running on an agent that has lost
    contact with the current master (typically due to a network partition or the
    agent host crashing). This case is described further below.
  * `TASK_ERROR` indicates that a task launch attempt failed because of an error
    in the task specification.

## Dealing with Partitioned or Failed Agents

The Mesos master tracks the availability and health of the registered agents
using two different mechanisms:

1. The state of a persistent TCP connection between the master and the agent.

2. Health checks via periodic ping messages to the agent which are expected to
   be responded with pongs (this behavior is controlled by the
   `--slave_ping_timeout` and `--max_slave_ping_timeouts` master flags).

If the persistent TCP connection to the agent breaks or the agent fails health
checks, the master decides that the agent has failed and takes steps to remove
it from the cluster. Specifically:

* If the TCP connection breaks, the agent is considered disconnected. The
  semantics when a registered agent gets disconnected are as follows for each
  framework running on that agent:

  * If the framework is [checkpointing](slave-recovery.md): no immediate action
    is taken. The agent is given a chance to reconnect until health checks time
    out.

  * If the framework is not checkpointing: all the framework's tasks and
    executors are considered lost. The master immediately sends `TASK_LOST`
    status updates for the tasks. These updates are not delivered reliably to
    the scheduler (see NOTE below). The agent is given a chance to reconnect
    until health checks timeout.

* If the agent fails health checks it is scheduled for removal. The removals can
  be rate limited by the master (see `---slave_removal_rate_limit` master flag)
  to avoid removing a slew of slaves at once (e.g., during a network partition).

* Once it is time to remove an agent, the master marks it as "removed" in the
  master's durable state (this will survive master failover). If an agent marked
  as "removed" attempts to reconnect to the master (e.g., after a network
  partition is healed), the connection attempt will be refused and the agent
  will be asked to shutdown. The agent will then shutdown all running tasks and
  executors, but any persistent volumes and dynamic reservations will be
  preserved.

  * To allow the removed agent node to rejoin the cluster, a new `mesos-slave`
    process can be started. This will ensure the agent will receive a new agent
    ID. The agent can then register with the master, and can also retain any
    previously created persistent volumes and dynamic reservations. In effect,
    the agent will be treated as a newly joined agent.

* For each agent that is marked "removed", the scheduler receives a `slaveLost`
  callback. The scheduler will also receive `TASK_LOST` status updates for each
  task that was running on a removed agent.

    >NOTE: Neither the callback nor the updates are reliably delivered by the
    master. For example, if the master or scheduler fails over or there is a
    network connectivity issue during the delivery of these messages, they will
    not be resent.

Typically, frameworks respond to failed or partitioned agents by scheduling new
copies of the tasks that were running on the lost agent. This should be done
with caution, however: it is possible that the lost agent is still alive, but is
partitioned from the master and is unable to communicate with it. Depending on
the nature of the network partition, tasks on the agent might still be able to
communicate with external clients or other hosts in the cluster. Frameworks can
take steps to prevent this (e.g., by having tasks connect to ZooKeeper and cease
operation if their ZooKeeper session expires), but Mesos leaves such details to
framework authors.

## Dealing with Partitioned or Failed Masters

The behavior described above does not apply during the period immediately after
a new Mesos master is elected. As noted above, most Mesos master state is kept
in-memory; hence, when the leading master fails and a new master is elected, the
new master will have little knowledge of the current state of the cluster.
Instead, it rebuilds this information as the frameworks and agents notice that a
new master has been elected and then _reregister_ with it.

### Framework Reregistration
When master failover occurs, frameworks that were connected to the previous
leading master should reconnect to the new leading master. The
`MesosSchedulerDriver` handles most of the details of detecting when the
previous leading master has failed and connecting to the new leader; when the
framework has successfully reregistered with the new leading master, the
`reregistered` scheduler callback will be invoked.

### Agent Reregistration
During the period after a new master has been elected but before a given agent
has reregistered or the `slave_reregister_timeout` has fired, attempting to
reconcile the state of a task running on that agent will not return any
information (because the master cannot accurately determine the state of the
task).

If an agent does not reregister with the new master within a timeout (controlled
by the `--slave_reregister_timeout` configuration flag), the master marks the
agent as failed and follows the same steps described above. However, there is
one difference: by default, agents are _allowed to reconnect_ following master
failover, even after the `slave_reregister_timeout` has fired. This means that
frameworks might see a `TASK_LOST` update for a task but then later discover
that the task is running (because the agent where it was running was allowed to
reconnect). This behavior can be avoided by enabling the `--registry_strict`
configuration flag, which will be the default in a future version of Mesos.
