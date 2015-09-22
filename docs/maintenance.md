---
layout: documentation
---

# Maintenance Primitives

Operators regularly need to perform maintenance tasks on machines that comprise
a Mesos cluster.  Most Mesos upgrades can be done without affecting running
tasks, but there are situations where maintenance may affect running tasks.
For example:

* Hardware repair
* Kernel upgrades
* Agent upgrades (e.g. adjusting agent attributes or resources)

Frameworks require visibility into any actions that disrupt cluster operation
in order to meet Service Level Agreements or to ensure uninterrupted services
for their end users.  Therefore, to reconcile the requirements of frameworks
and operators, frameworks must be aware of planned maintenance events and
operators must be aware of frameworks’ ability to adapt to maintenance.
Maintenance primitives add a layer to facilitate communication between the
frameworks and operator.

## Terminology

For the purpose of this document, an "Operator" is a person, tool, or script
which manages the Mesos cluster.

Maintenance primitives add several new concepts to the Mesos cluster.
Those concepts are:

* **Maintenance** - An operation that makes resources on a machine unavailable,
  either temporarily or permanently.
* **Maintenance window** - A set of machines and an associated interval during
  which some maintenance is planned on those machines.
* **Maintenance schedule** - A list of maintenance windows.
  A single machine may only appear in a schedule once.
* **Unavailability** - An operator-specified interval, defined by a start time
  and duration, during which an associated machine may become unavailable.
  In general, no assumptions should be made about the availability of the
  machine (or resources) after the unavailability.
* **Drain** - An interval between the scheduling of maintenance and when the
  machine(s) become unavailable.  Offers sent with resources from draining
  machines will contain unavailability information.  Frameworks running on
  draining machines will receive inverse offers (see next).  Frameworks
  utilizing resources on affected machines are expected either to take
  preemptive steps to prepare for the unavailability; or to communicate the
  framework's inability to conform to the maintenance schedule.
* **Inverse offer** - A communication mechanism for the master to ask for
  resources back from a framework.  This notifies frameworks about any
  unavailability and gives frameworks a mechanism to respond about their
  capability to comply.  Inverse offers are similar to offers in that they
  can be accepted, rejected, re-offered, and rescinded.

**Note**: Unavailability and inverse offers are not specific to maintenance.
The same concepts can be used for non-maintenance goals, such as reallocating
resources or resource preemption.

## How does it work?

Maintenance primitives were introduced in Mesos 0.25.0.  Several machine
maintenance modes were also introduced.  Those modes are illustrated below.

![Maintenance mode transitions](images/maintenance-primitives-modes.png)

All mode transitions must be initiated by the operator.  Mesos will not
change the mode of any machine, regardless of the estimate provided in
the maintenance schedule.

### Scheduling maintenance

A machine is transitioned from Up mode into Draining mode as soon as it
is scheduled for maintenance.  To transition a machine into Draining mode,
an operator constructs a maintenance schedule and posts it to the Mesos master.

See the definition of a [maintenance::Schedule](https://github.com/apache/mesos/blob/016b02d7ed5a65bcad9261a133c8237c2df66e6e/include/mesos/maintenance/maintenance.proto#L48-L67)
and of [Unavailability](https://github.com/apache/mesos/blob/016b02d7ed5a65bcad9261a133c8237c2df66e6e/include/mesos/v1/mesos.proto#L140-L154).

In a production environment, the schedule must be constructed to ensure that
enough agents are operational at any given point in time to ensure
uninterrupted service by the frameworks.

For example, in a cluster of three machines, the operator can schedule two
machines for one hour of maintenance, followed by another hour for the last
machine.  The timestamps for unavailability are in nanoseconds since the epoch.
The schedule might look like:

```
{
  "windows" : [
    {
      "machine_ids" : [
        { "hostname" : "machine1", "ip" : "10.0.0.1" },
        { "hostname" : "machine2", "ip" : "10.0.0.2" }
      ],
      "unavailability" : {
        "start" : { "nanoseconds" : 1443830400000000000 },
        "duration" : { "nanoseconds" : 3600000000000 }
      }
    }, {
      "machine_ids" : [
        { "hostname" : "machine3", "ip" : "10.0.0.3" }
      ],
      "unavailability" : {
        "start" : { "nanoseconds" : 1443834000000000000 },
        "duration" : { "nanoseconds" : 3600000000000 }
      }
    }
  ]
}
```

The operator then posts the schedule to the master's maintenance endpoints.

```
curl http://localhost:5050/master/maintenance/schedule
  -H "Content-type: application/json"
  -X POST
  -d @schedule.json
```

The machines in a maintenance schedule do not necessarily need to be registered
with the Mesos master.  The operator may add a machine to the maintenance
schedule prior to launching an agent on the machine.  For example, this is
useful for preventing a faulty machine from launching an agent on boot.

**Note**: Each machine in the maintenance schedule should have as
complete information as possible.  In order for Mesos to recognize an agent
as coming from a particular machine, both the `hostname` and `ip` fields must
match.  Any omitted data defaults to the empty string `""`.  If there are
multiple hostnames or IPs for a machine, the machine's fields need to match
what the agent announces to the master.  If there is any ambiguity in a
machine's configuration, the operator should use the `--hostname` and `--ip`
options when starting agents.

The master checks that a maintenance schedule has the following properties:
* Each maintenance window in the schedule must have at least one machine
  and a specified unavailability interval.
* Each machine must only appear in the schedule once.
* Each machine must have at least a hostname or IP included.
  The hostname is not case-sensitive.
* All machines that are in Down mode must be present in the schedule.
  This is required because this endpoint does not handle the transition
  from Down mode to Up mode.

If any of these properties are not met, the maintenance schedule is rejected
with a corresponding error message and the master's state does not change.

To update a maintenance schedule, the operator should first read the existing
schedule, make the necessary changes, and then post the modified schedule.

To cancel a maintenance schedule, the operator should post an empty schedule.

### Draining mode

As soon as a schedule is posted to the Mesos master, the following things occur:

* The schedule is stored in the replicated log.  This means
  the schedule is persisted in case of master failover.
* All machines in the schedule are immediately transitioned into Draining
  mode.  The mode of each machine is also persisted in the replicated log.
* All frameworks using resources on affected agents are immediately
  notified.  Existing offers from the affected agents are rescinded
  and re-sent with additional unavailability data.  All Frameworks using
  resources from the affected agents are given inverse offers.
* New offers from the affected agents will also include
  the additional unavailability data.

With this additional information, frameworks should perform scheduling in a
maintenance-aware fashion.  Inverse offers communicate the frameworks' ability
to conform to the maintenance schedule.
For example:

* A framework with long-running tasks may choose agents with no unavailability
  or with unavailability further in the future.
* A datastore may choose to start a new replica if one of its agents is
  scheduled for extensive maintenance or decommissioning.  If the datastore
  can reasonably copy data into a new agent before maintenance,
  it would accept any inverse offers.  Otherwise, it would decline them.
* A stateful task, on an eminently unavailable agent, may be migrated to
  another available agent.  If the framework has sufficient resources to do
  so, it would accept any inverse offers.  Otherwise, it would decline them.

Accepting an inverse offer indicates that the framework is ok with the
maintenance schedule as it currently stands, given the current state of
the framework's resources.  The master and operator should perceive acceptance
as a best-effort promise by the framework to free all the resources contained
in the inverse offer by the start of the unavailability interval.  An inverse
offer may also be rejected if the framework is unable to conform to the
maintenance schedule.

A framework can use a filter to control when it wants to be contacted again
with an inverse offer.  This is useful since future circumstances may change
the viability of the maintenance schedule.  The filter for inverse offers is
identical to the existing mechanism for re-offering offers to frameworks.

**Note**: Accepting or declining an inverse offer does not result in
immediate changes in the maintenance schedule, or in the way Mesos acts.
Inverse offers only represent some extra information that frameworks may
find useful. In the same manner, a rejection or acceptance of an offer is a
hint for an operator. The operator may or may not chose to take that hint
into account.

### Starting maintenance

The operator starts maintenance by posting a list of machines to the master's
maintenance endpoint.

See the definition of a [MachineID](https://github.com/apache/mesos/blob/016b02d7ed5a65bcad9261a133c8237c2df66e6e/include/mesos/v1/mesos.proto#L157-L167).

For example, to start maintenance on two machines:

```
[
  { "hostname" : "machine1", "ip" : "10.0.0.1" },
  { "hostname" : "machine2", "ip" : "10.0.0.2" }
]
```

```
curl http://localhost:5050/master/machine/down
  -H "Content-type: application/json"
  -X POST
  -d @machines.json
```

The master checks that a list of machines has the following properties:

* The list of machines must not be empty.
* Each machine must only appear once.
* Each machine must have at least a hostname or IP included.
  The hostname is not case-sensitive.
* If a machine's IP is included, it must be correctly formed.
* All listed machines must be present in the schedule.

If any of these properties are not met, the list of machines is rejected
with a corresponding error message and the master's state does not change.

The operator can start maintenance on any machine that is scheduled for
maintenance.  Machines that are not scheduled for maintenance cannot be
directly transitioned from Up mode into Down mode.  However, the operator
may schedule a machine for maintenance with a timestamp of the current
time or in the past; and then immediately start maintenance on that machine.

It is up to the operator to transition a machine from Draining to Deactivated
mode.  Mesos will keep a machine in Draining mode even if the unavailability
window arrives or passes.  This means that the operation of the machine is not
disrupted in any way and offers (with unavailability information) are still
sent for this machine.

When maintenance is triggered by the operator, all agents on the machine are
told to shutdown.  These agents are subsequently removed from the master
which causes tasks to be updated as `TASK_LOST`.  Any agents from
machines in maintenance are also prevented from registering with the master.

### Completing maintenance

When maintenance is complete, or if maintenance needs to be cancelled,
the operator can stop maintenance.  The process is very similar
to starting maintenance (same validation criterion as the previous section).
The operator posts a list of machines to the master's endpoints:

```
[
  { "hostname" : "machine1", "ip" : "10.0.0.1" },
  { "hostname" : "machine2", "ip" : "10.0.0.2" }
]
```

```
curl http://localhost:5050/master/machine/up
  -H "Content-type: application/json"
  -X POST
  -d @machines.json
```

**Note**: The duration of the maintenance, as indicated by the "unavailability"
field, is a best-effort guess made by the operator.  Stopping maintenance
before the end of the unavailability interval is allowed, as is stopping
maintenance after the end of the unavailability interval.  The machines are
never automatically transitioned out of maintenance.

Frameworks are informed about the completion or cancellation of maintenance
when offers from that machine start being sent.  There is no explicit mechanism
for notifying frameworks when maintenance is stopped.  After maintenance is
stopped, new offers are no longer tagged with unavailability and inverse offers
are no longer sent.  Also, new agents can start to register from the machine.
