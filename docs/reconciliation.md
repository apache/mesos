---
title: Apache Mesos - Reconciliation
layout: documentation
---

# Task Reconciliation

Messages between framework schedulers and the Mesos master may be dropped due to
failures and network partitions. This may cause a framework scheduler and the
master to have different views of the current state of the cluster. For example,
consider a launch task request sent by a framework.  There are many ways that
failures can prevent the task launch operation from succeeding, such as:

* Framework fails after persisting its intent to launch the task, but
before the launch task message was sent.
* Master fails before receiving the message.
* Master fails after receiving the message but before sending it to the
agent.

In these cases, the framework believes the task to be staging but the task is
unknown to the master. To cope with such situations, Mesos frameworks should use
*reconciliation* to ask the master for the current state of their tasks.

## How To Reconcile

Frameworks can use the scheduler driver's `reconcileTasks` method to send a
reconciliation request to the master:

~~~{.cpp}
// Allows the framework to query the status for non-terminal tasks.
// This causes the master to send back the latest task status for
// each task in 'statuses', if possible. Tasks that are no longer
// known will result in a TASK_LOST update. If statuses is empty,
// then the master will send the latest status for each task
// currently known.
virtual Status reconcileTasks(const std::vector<TaskStatus>& statuses);
~~~

Currently, the master will only examine two fields in `TaskStatus`:

* `TaskID`: This is required.
* `SlaveID`: Optional but recommended. This leads to faster reconciliation in
the presence of agents that are transitioning between states.

Mesos provides two forms of reconciliation:

* "Explicit" reconciliation: the scheduler sends a list of non-terminal task IDs
  and the master responds with the latest state for each task, if possible.
* "Implicit" reconciliation: the scheduler sends an empty list of tasks
  and the master responds with the latest state for all currently known
  non-terminal tasks.

Reconciliation results are returned as task status updates (e.g., via the
scheduler driver's `statusUpdate` callback). Status updates that result from
reconciliation requests will their `reason` field set to
`REASON_RECONCILIATION`. Note that most of the other fields in the returned
`TaskStatus` message will not be set: for example, reconciliation cannot be used
to retrieve the `labels` or `data` fields associated with a running task.

## When To Reconcile

Framework schedulers should periodically reconcile *all* of their tasks (for
example, every fifteen minutes). This serves two purposes:

  1. It is necessary to account for dropped messages between the framework and
     the master; for example, see the task launch scenario described above.
  2. It is a defensive programming technique to catch bugs in both the framework
     and the Mesos master.

As an optimization, framework schedulers should reconcile _more frequently_ when
they have reason to suspect that their local state differs from that of the
master. For example, after a framework launches a task, it should expect to
receive a `TASK_RUNNING` status update for the new task fairly promptly. If no
such update is received, the framework should perform explicit reconciliation
more quickly than usual.

Similarly, frameworks should initiate reconciliation after both framework
failovers and master failovers. Note that the scheduler driver notifies
frameworks when master failover has occurred (via the `reregistered()`
callback). For more information, see the
[guide to designing highly available frameworks](high-availability-framework-guide.md).

## Algorithm

This technique for explicit reconciliation reconciles all non-terminal tasks
until an update is received for each task, using exponential backoff to retry
tasks that remain unreconciled. Retries are needed because the master temporarily
may not be able to reply for a particular task. For example, during master
failover the master must reregister all of the agents to rebuild its
set of known tasks (this process can take minutes for large clusters, and
is bounded by the `--agent_reregister_timeout` flag on the master).

Steps:

1. let `start = now()`
2. let `remaining = { T in tasks | T is non-terminal }`
3. Perform reconciliation: `reconcile(remaining)`
4. Wait for status updates to arrive (use truncated exponential backoff). For each update, note the time of arrival.
5. let `remaining = { T in remaining | T.last_update_arrival() < start }`
6. If `remaining` is non-empty, go to 3.

This reconciliation algorithm **must** be run after each (re-)registration.

Implicit reconciliation (passing an empty list) should also be used
periodically, as a defense against data loss in the framework. Unless a
strict registry is in use on the master, its possible for tasks to resurrect
from a LOST state (without a strict registry the master does not enforce
agent removal across failovers). When an unknown task is encountered, the
scheduler should kill or recover the task.

Notes:

* When waiting for updates to arrive, **use a truncated exponential backoff**.
This will avoid a snowball effect in the case of the driver or master being
backed up.
* It is beneficial to ensure that only 1 reconciliation is in progress at a
time, to avoid a snowball effect in the face of many re-registrations.
If another reconciliation should be started while one is in-progress,
then the previous reconciliation algorithm should stop running.


# Offer Reconciliation

Offers are reconciled automatically after a failure:

* Offers do not persist beyond the lifetime of a Master.
* If a disconnection occurs, offers are no longer valid.
* Offers are rescinded and regenerated each time the framework (re-)registers.


# Operation Reconciliation

When a scheduler specifies an `id` on an offer operation, the master will
provide updates on the status of that operation. If the scheduler needs to
reconcile its view of the current states of operations with the master's view,
it can do so via the `RECONCILE_OPERATIONS` call in the v1 scheduler API.

Operation reconciliation is similar to task reconciliation in that the scheduler
can perform either explicit or implicit reconciliation by specifying particular
operation IDs or by leaving the `operations` field unset, respectively.

In order to explicitly reconcile particular operations, the scheduler should
include in the `RECONCILE_OPERATIONS` call a list of operations, specifying an
operation ID, agent ID, and resource provider ID (if applicable) for each one.
While the agent and resource provider IDs are optional, the master will be able
to provide the highest quality reconciliation information when they are set. For
example, if the relevant agent is not currently registered, inclusion of the
agent ID will allow the master to respond with states like
`OPERATION_RECOVERING`, `OPERATION_UNREACHABLE`, or `OPERATION_GONE_BY_OPERATOR`
when the agent is recovering, unreachable, or gone, respectively. Inclusion of
the resource provider ID provides the same benefit for cases where the
resource provider is recovering or gone.

Similar to task reconciliation, we recommend that schedulers implement a
periodic reconciliation loop for operations in order to defend against network
failures and bugs in the scheduler and/or Mesos master.
