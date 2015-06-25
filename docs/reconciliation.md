# Reconciliation

There's no getting around it, **frameworks on Mesos are distributed systems**.

**Distributed systems must deal with failures**, and partitions (the two are
indistinguishable from a system's perspective).

Concretely, what does this mean for frameworks? Mesos uses an actor-like
**message passing programming model, in which messages are delivered
at-most-once**. (Exceptions to this include task status updates, most of
which are delivered at-least-once through the use of acknowledgements).
**The messages passed between the master and the framework are therefore
susceptible to be dropped, in the presence of failures**.

When these non-reliable messages are dropped, inconsistent state can arise
between the framework and Mesos.

As a simple example, consider a launch task request sent by a framework.
There are many ways that failures can lead to the loss of the task, for
example:

* Framework fails after persisting its intent to launch the task, but
before the launch task message was sent.
* Master fails before receiving the message.
* Master fails after receiving the message, but before sending it to the
slave.

In these cases, the framework believes the task to be staging, but the
task is unknown to Mesos. To cope with such situations, **task state must be
reconciled between the framework and Mesos whenever a failure is detected**.


## Detecting Failures

It is the responsibility of Mesos (scheduler driver / Master) to ensure
that the framework is notified when a disconnection, and subsequent
(re-)registration occurs. At this point, the scheduler should perform
task state reconciliation.


## Task Reconciliation

**Tasks must be reconciled explicitly by the framework after a failure.**

This is because the scheduler driver does not persist any task information.
In the future, the scheduler driver (or a pure-language mesos library) could
perform task reconciliation seamlessly under the covers on behalf of the
framework.

So, for now, let's look at how one needs to implement task state
reconciliation in a framework scheduler.

### API

Frameworks send a list of `TaskStatus` messages to the master:

    // Allows the framework to query the status for non-terminal tasks.
    // This causes the master to send back the latest task status for
    // each task in 'statuses', if possible. Tasks that are no longer
    // known will result in a TASK_LOST update. If statuses is empty,
    // then the master will send the latest status for each task
    // currently known.
    message Reconcile {
      repeated TaskStatus statuses = 1; // Should be non-terminal only.
    }


Currently, the master will only examine two fields in `TaskStatus`:

* `TaskID`: This is required.
* `SlaveID`: Optional, leads to faster reconciliation in the presence of
slaves that are transitioning between states.

### Algorithm

The technique for performing reconciliation should reconcile all non-terminal
tasks, until an update is received for each task, using exponential backoff:

1. let `start = now()`
2. let `remaining = { T ϵ tasks | T is non-terminal }`
3. Perform reconciliation: `reconcile(remaining)`
4. Wait for status updates to arrive (use truncated exponential backoff). For each update, note the time of arrival.
5. let `remaining = { T ϵ remaining | T.last_update_arrival() < start }`
6. If `remaining` is non-empty, go to 3.

This reconciliation algorithm **must** be run after each (re-)registration.

Notes:

* When waiting for updates to arrive, **use a truncated exponential backoff**.
This will avoid a snowball effect in the case of the driver or master being
backed up.
* Implicit reconciliation (passing an empty list) can also be used
periodically, As a defense against data loss in the framework.
* It is beneficial to ensure that only 1 reconciliation is in progress at a
time, to avoid a snowball effect in the face of many re-registrations.
If another reconciliation should be started while one is in-progress,
then the previous reconciliation algorithm should stop running.


## Offer Reconciliation

Offers are reconciled automatically after a failure:

* Offers do not persist beyond the lifetime of a Master.
* If a disconnection occurs, offers are no longer valid.
* Offers are rescinded and regenerated each time the framework (re-)registers.
