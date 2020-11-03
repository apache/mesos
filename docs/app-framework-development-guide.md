---
title: Apache Mesos - Framework Development Guide
layout: documentation
---

# Framework Development Guide

In this document we refer to Mesos applications as "frameworks".

See one of the example framework schedulers in `MESOS_HOME/src/examples/` to
get an idea of what a Mesos framework scheduler and executor in the language
of your choice looks like. [RENDLER](https://github.com/mesosphere/RENDLER)
provides example framework implementations in C++, Go, Haskell, Java, Python
and Scala.

## Create your Framework Scheduler

### API
If you are writing a scheduler against Mesos 1.0 or newer, it is recommended
to use the new [HTTP API](scheduler-http-api.md) to talk to Mesos.

If your framework needs to talk to Mesos 0.28.0 or older, or you have not updated to the
[HTTP API](scheduler-http-api.md), you can write the scheduler in C++, Java/Scala, or Python.
Your framework scheduler should inherit from the `Scheduler` class
(see: [C++](https://github.com/apache/mesos/blob/1.6.0/include/mesos/scheduler.hpp#L58-L177),
[Java](http://mesos.apache.org/api/latest/java/org/apache/mesos/Scheduler.html),
[Python](https://github.com/apache/mesos/blob/1.6.0/src/python/interface/src/mesos/interface/__init__.py#L34-L137)). Your scheduler should create a SchedulerDriver (which will mediate
communication between your scheduler and the Mesos master) and then call `SchedulerDriver.run()`
(see: [C++](https://github.com/apache/mesos/blob/1.6.0/include/mesos/scheduler.hpp#L180-L317),
[Java](http://mesos.apache.org/api/latest/java/org/apache/mesos/SchedulerDriver.html),
[Python](https://github.com/apache/mesos/blob/1.6.0/src/python/interface/src/mesos/interface/__init__.py#L140-L278)).

### High Availability

How to build Mesos frameworks that are highly available in the face of failures is
discussed in a [separate document](high-availability-framework-guide.md).

### Multi-Scheduler Scalability

When implementing a scheduler, it's important to adhere to the following guidelines
in order to ensure that the scheduler can run in a scalable manner alongside other
schedulers in the same Mesos cluster:

1. **Use `Suppress`**: The scheduler must stay in a suppressed state whenever it has
   no additional tasks to launch or offer operations to perform. This ensures
   that Mesos can more efficiently offer resources to those frameworks that do
   have work to perform.
2. **Do not hold onto offers**: If an offer cannot be used, decline it immediately.
   Otherwise the resources cannot be offered to other schedulers and the scheduler
   itself will receive fewer additional offers.
3. **`Decline` resources using a large timeout**: when declining an offer, use a
   large `Filters.refuse_seconds` timeout (e.g. 1 hour). This ensures that Mesos
   will have time to try offering the resources to other scheduler before trying
   the same scheduler again. However, if the scheduler is unable to eventually
   enter a `SUPPRESS`ed state, and it has new workloads to run after having declined,
   it should consider `REVIVE`ing if it is not receiving sufficient resources for
   some time.
4. **Do not `REVIVE` frequently**: `REVIVE`ing clears all filters, and therefore
   if `REVIVE` occurs frequently it is similar to always declining with a very
   short timeout (violation of guideline (3)).
5. **Use `FrameworkInfo.offer_filters`**: This allows the scheduler to specify
   global offer filters (`Decline` filters, on the other hand, are per-agent).
   Currently supported is `OfferFilters.min_allocatable_resources` which acts as
   an override of the cluster level `--min_allocatable_resources` master flag for
   each of the scheduler's roles. Keeping the `FrameworkInfo.offer_filters`
   up-to-date with the minimum desired offer shape for each role will ensure that
   the sccheduler gets a better chance to receive offers sized with sufficient
   resources.
6. Consider specifying **offer constraints** via `SUBSCRIBE`/`UPDATE_FRAMEWORK`
   calls so that the framework role's quota is not consumed by offers that the
   scheduler will have to decline anyway based on agent attributes.
   See [MESOS-10161](https://issues.apache.org/jira/browse/MESOS-10161])
   and [scheduler.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/scheduler/scheduler.proto)
   for more details.

Operationally, the following can be done to ensure that schedulers get the resources
they need when co-existing with other schedulers:

1. **Do not share a role between schedulers**: Roles are the level at which controls
   are available (e.g. quota, weight, reservation) that affect resource allocation.
   Within a role, there are no controls to alter the behavior should one scheduler
   not receive enough resources.
2. **Set quota if roles need a guarantee**: If a role (either an entire scheduler or
   a "job"/"service"/etc within a multi-tenant scheduler) needs a certain amount of
   resources guaranteed to it, setting a quota ensures that Mesos will try its best
   to allocate to satisfy the guarantee.
3. **Set the minimum allocatable resources**: Once quota is used, the
   `--min_allocatable_resources` flag should be set
   (e.g. `--min_allocatable_resources=cpus:0.1,mem:32:disk:32`) to prevent offers
   that are missing cpu, memory, or disk
   (see [MESOS-8935](https://issues.apache.org/jira/browse/MESOS-8935)).
4. **Consider enabling the random sorter**: Depending on the use case, DRF can prove
   problematic in that it will try to allocate to frameworks with a low share of the
   cluster and penalize frameworks with a high share of the cluster. This can lead
   to offer starvation for higher share frameworks. To allocate using a weighted
   random uniform distribution instead of fair sharing, set `--role_sorter=random`
   and `--framework_sorter=random` (see
   [MESOS-8936](https://issues.apache.org/jira/browse/MESOS-8936)).

See the [Offer Starvation Design Document](https://docs.google.com/document/d/1uvTmBo_21Ul9U_mijgWyh7hE0E_yZXrFr43JIB9OCl8)
in [MESOS-3202](https://issues.apache.org/jira/browse/MESOS-3202) for more
information about the pitfalls and future plans for running multiple schedulers.

## Working with Executors

### Using the Mesos Command Executor

Mesos provides a simple executor that can execute shell commands and Docker
containers on behalf of the framework scheduler; enough functionality for a
wide variety of framework requirements.

Any scheduler can make use of the Mesos command executor by filling in the
optional `CommandInfo` member of the `TaskInfo` protobuf message.

~~~{.proto}
message TaskInfo {
  ...
  optional CommandInfo command = 7;
  ...
}
~~~

The Mesos slave will fill in the rest of the `ExecutorInfo` for you when tasks
are specified this way.

Note that the agent will derive an `ExecutorInfo` from the `TaskInfo` and
additionally copy fields (e.g., `Labels`) from `TaskInfo` into the new
`ExecutorInfo`. This `ExecutorInfo` is only visible on the agent.


### Using the Mesos Default Executor

Since Mesos 1.1, a new built-in default executor (**experimental**) is available that
can execute a group of tasks. Just like the command executor the tasks can be shell
commands or Docker containers.

The current semantics of the default executor are as folows:

-- Task group is an atomic unit of deployment of a scheduler onto the default executor.

-- The default executor can run one or more task groups (since Mesos 1.2) and each task group can be launched by the scheduler at different points in time.

-- All task groups' tasks are launched as nested containers underneath the executor container.

-- Task containers and executor container share resources like cpu, memory,
   network and volumes.
   
-- Each task can have its own separate root file system (e.g., Docker image).

-- There is no resource isolation between different tasks or task groups within an executor.
   Tasks' resources are added to the executor container.

-- If any of the tasks exits with a non-zero exit code or killed by the scheduler, all the tasks in the task group
   are killed automatically. The default executor commits suicide if there are no active task groups.
   

Once the default executor is considered **stable**, the command executor will be deprecated in favor of it.

Any scheduler can make use of the Mesos default executor by setting `ExecutorInfo.type`
to `DEFAULT` when launching a group of tasks using the `LAUNCH_GROUP` offer operation.
If `DEFAULT` executor is explicitly specified when using `LAUNCH` offer operation, command
executor is used instead of the default executor. This might change in the future when the default
executor gets support for handling `LAUNCH` operation.


~~~{.proto}
message ExecutorInfo {
  ...
    optional Type type = 15;
  ...
}
~~~


### Creating a custom Framework Executor

If your framework has special requirements, you might want to provide your own
Executor implementation. For example, you may not want a 1:1 relationship
between tasks and processes.

If you are writing an executor against Mesos 1.0 or newer, it is recommended
to use the new [HTTP API](executor-http-api.md) to talk to Mesos.

If writing against Mesos 0.28.0 or older, your framework executor must inherit
from the Executor class (see (see: [C++](https://github.com/apache/mesos/blob/1.6.0/include/mesos/executor.hpp#L60-L137),
[Java](http://mesos.apache.org/api/latest/java/org/apache/mesos/Executor.html),
[Python](https://github.com/apache/mesos/blob/1.6.0/src/python/interface/src/mesos/interface/__init__.py#L280-L344)). It must override the launchTask() method. You can use
the $MESOS_HOME environment variable inside of your executor to determine where
Mesos is running from. Your executor should create an ExecutorDriver (which will
mediate communication between your executor and the Mesos agent) and then call
`ExecutorDriver.run()`
(see: [C++](https://github.com/apache/mesos/blob/1.6.0/include/mesos/executor.hpp#L140-L188),
[Java](http://mesos.apache.org/api/latest/java/org/apache/mesos/ExecutorDriver.html),
[Python](https://github.com/apache/mesos/blob/1.6.0/src/python/interface/src/mesos/interface/__init__.py#L348-L401)).

#### Install your custom Framework Executor

After creating your custom executor, you need to make it available to all slaves
in the cluster.

One way to distribute your framework executor is to let the
[Mesos fetcher](fetcher.md) download it on-demand when your scheduler launches
tasks on that slave. `ExecutorInfo` is a Protocol Buffer Message class (defined
in `include/mesos/mesos.proto`), and it contains a field of type `CommandInfo`.
`CommandInfo` allows schedulers to specify, among other things, a number of
resources as URIs. These resources are fetched to a sandbox directory on the
slave before attempting to execute the `ExecutorInfo` command. Several URI
schemes are supported, including HTTP, FTP, HDFS, and S3 (e.g. see
src/examples/java/TestFramework.java for an example of this).

Alternatively, you can pass the `frameworks_home` configuration option
(defaults to: `MESOS_HOME/frameworks`) to your `mesos-slave` daemons when you
launch them to specify where your framework executors are stored (e.g. on an
NFS mount that is available to all slaves), then use a relative path in
`CommandInfo.uris`, and the slave will prepend the value of `frameworks_home`
to the relative path provided.

Once you are sure that your executors are available to the mesos-slaves, you
should be able to run your scheduler, which will register with the Mesos master,
and start receiving resource offers!


## Labels

`Labels` can be found in the `FrameworkInfo`, `TaskInfo`, `DiscoveryInfo` and
`TaskStatus` messages; framework and module writers can use Labels to tag and
pass unstructured information around Mesos. Labels are free-form key-value pairs
supplied by the framework scheduler or label decorator hooks. Below is the
protobuf definitions of labels:

~~~{.proto}
  optional Labels labels = 11;
~~~

~~~{.proto}
/**
 * Collection of labels.
 */
message Labels {
    repeated Label labels = 1;
}

/**
 * Key, value pair used to store free form user-data.
 */
message Label {
  required string key = 1;
  optional string value = 2;
}
~~~

Labels are not interpreted by Mesos itself, but will be made available over
master and slave state endpoints. Further more, the executor and scheduler can
introspect labels on the `TaskInfo` and `TaskStatus` programmatically.
Below is an example of how two label pairs (`"environment": "prod"` and
`"bananas": "apples"`) can be fetched from the master state endpoint.


~~~{.sh}
$ curl http://master/state.json
...
{
  "executor_id": "default",
  "framework_id": "20150312-120017-16777343-5050-39028-0000",
  "id": "3",
  "labels": [
    {
      "key": "environment",
      "value": "prod"
    },
    {
      "key": "bananas",
      "value": "apples"
    }
  ],
  "name": "Task 3",
  "slave_id": "20150312-115625-16777343-5050-38751-S0",
  "state": "TASK_FINISHED",
  ...
},
~~~

## Service discovery

When your framework registers an executor or launches a task, it can provide
additional information for service discovery. This information is stored by
the Mesos master along with other imporant information such as the slave
currently running the task. A service discovery system can programmatically
retrieve this information in order to set up DNS entries, configure proxies,
or update any consistent store used for service discovery in a Mesos cluster
that runs multiple frameworks and multiple tasks.

The optional `DiscoveryInfo` message for `TaskInfo` and `ExecutorInfo` is
declared in  `MESOS_HOME/include/mesos/mesos.proto`

~~~{.proto}
message DiscoveryInfo {
  enum Visibility {
    FRAMEWORK = 0;
    CLUSTER = 1;
    EXTERNAL = 2;
  }

  required Visibility visibility = 1;
  optional string name = 2;
  optional string environment = 3;
  optional string location = 4;
  optional string version = 5;
  optional Ports ports = 6;
  optional Labels labels = 7;
}
~~~

`Visibility` is the key parameter that instructs the service discovery system
whether a service should be discoverable. We currently differentiate between
three cases:

 - a task should not be discoverable for anyone but its framework.
 - a task should be discoverable for all frameworks running on the Mesos cluster
   but not externally.
 - a task should be made discoverable broadly.

Many service discovery systems provide additional features that manage the
visibility of services (e.g., ACLs in proxy based systems, security extensions
to DNS, VLAN or subnet selection). It is not the intended use of the visibility
field to manage such features. When a service discovery system retrieves the
task or executor information from the master, it can decide how to handle tasks
without `DiscoveryInfo`. For instance, tasks may be made non discoverable to
other frameworks (equivalent to `visibility=FRAMEWORK`) or discoverable to all
frameworks (equivalent to `visibility=CLUSTER`).

The `name` field is a string that provides the service discovery system
with the name under which the task is discoverable. The typical use of the name
field will be to provide a valid hostname. If name is not provided, it is up to
the service discovery system to create a name for the task based on the name
field in `taskInfo` or other information.

The `environment`, `location`, and `version` fields provide first class support
for common attributes used to differentiate between similar services in large
deployments. The `environment` may receive values such as `PROD/QA/DEV`, the
`location` field may receive values like `EAST-US/WEST-US/EUROPE/AMEA`, and the
`version` field may receive values like v2.0/v0.9. The exact use of these fields
is up to the service discovery system.

The `ports` field allows the framework to identify the ports a task listens to
and explicitly name the functionality they represent and the layer-4 protocol
they use (TCP, UDP, or other). For example, a Cassandra task will define ports
like `"7000,Cluster,TCP"`, `"7001,SSL,TCP"`, `"9160,Thrift,TCP"`,
`"9042,Native,TCP"`, and `"7199,JMX,TCP"`. It is up to the service discovery
system to use these names and protocol in appropriate ways, potentially
combining them with the `name` field in `DiscoveryInfo`.

The `labels` field allows a framework to pass arbitrary labels to the service
discovery system in the form of key/value pairs. Note that anything passed
through this field is not guaranteed to be supported moving forward.
Nevertheless, this field provides extensibility. Common uses of this field will
allow us to identify use cases that require first class support.
