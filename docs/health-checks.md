---
title: Apache Mesos - Task Health Checking and Generalized Checks
layout: documentation
---

# Task Health Checking and Generalized Checks

Sometimes applications crash, misbehave, or become unresponsive. To detect and
recover from such situations, some frameworks (e.g.,
[Marathon](https://github.com/mesosphere/marathon/blob/v1.3.6/docs/docs/health-checks.md),
[Apache Aurora](https://aurora.apache.org/documentation/0.8.0/user-guide/#http-health-checking-and-graceful-shutdown))
implement their own logic for checking the health of their tasks. This is
typically done by having the framework scheduler send a "ping" request, e.g.,
via HTTP, to the host where the task is running and arranging for the task or
executor to respond to the ping. Although this technique is extremely useful,
there are several disadvantages in the way it is usually implemented:

* Each Apache Mesos framework uses its own API and protocol.
* Framework developers have to reimplement common functionality.
* Health checks originating from a scheduler generate extra network traffic if
  the task and the scheduler run on different nodes (which is usually the case);
  moreover, network failures between the task and the scheduler may make the
  latter think that the former is unhealthy, which might not be the case.
* Implementing health checks in the framework scheduler can be a performance
  bottleneck. If a framework is managing a large number of tasks, performing
  health checks for every task can cause scheduler performance problems.

To address the aforementioned problems, Mesos 1.2.0 introduced
[the Mesos-native health check design](#mesos-native-checking), defined
common API for [command](#command-health-checks),
[HTTP(S)](#http-health-checks), and [TCP](#tcp-health-checks) health checks,
and provided reference implementations for all built-in executors.

Mesos 1.4.0 introduced [a generalized check](#anatomy-of-a-check), which
delegates interpretation of a check result to the framework. This might be
useful, for instance, to track tasks' internal state transitions reliably
without Mesos taking action on them.

**NOTE:** Some functionality related to health checking was available prior to
1.2.0 release, however it was considered experimental.

**NOTE:** Mesos monitors each process-based task, including Docker containers,
using an equivalent of a `waitpid()` system call. This technique allows
detecting and reporting process crashes, but is insufficient for cases when the
process is still running but is not responsive.

This document describes supported check and health check types, touches on
relevant implementation details, and mentions limitations and caveats.


<a name="mesos-native-checking"></a>
## Mesos-native Task Checking

In contrast to the state-of-the-art "scheduler health check" pattern mentioned
above, Mesos-native checks run on the agent node: it is the executor
which performs checks and not the scheduler. This improves scalability but means
that detecting network faults or task availability from the outside world
becomes a separate concern. For instance, if the task is running on a
partitioned agent, it will still be (health) checked and---if the health checks
fail---might be terminated. Needless to say that due to the network partition,
all this will happen without the framework scheduler being notified.

Mesos checks and health checks are described in
[`CheckInfo`](https://github.com/apache/mesos/blob/cdb90b91ce8ce02d6163e5e2ee5b46fb797b1dee/include/mesos/mesos.proto#L403-L485)
and [`HealthCheck`](https://github.com/apache/mesos/blob/cdb90b91ce8ce02d6163e5e2ee5b46fb797b1dee/include/mesos/mesos.proto#L488-L589)
protobufs respectively. Currently, only tasks can be (health) checked, not
arbitrary processes or executors, i.e., only the `TaskInfo` protobuf has the
optional `CheckInfo` and `HealthCheck` fields. However, it is worth noting that
all built-in executors map a task to a process.

Task status updates are leveraged to transfer the check and health check status
to the Mesos master and further to the framework's scheduler ensuring the
"at-least-once" delivery guarantee. To minimize performance overhead, those task
status updates are triggered if a certain condition is met, e.g., the value or
presence of a specific field in the check status changes.

When a built-in executor sends a task status update because the check or health
check status has changed, it sets `TaskStatus.reason` to
`REASON_TASK_CHECK_STATUS_UPDATED` or `REASON_TASK_HEALTH_CHECK_STATUS_UPDATED`
respectively. While sending such an update, the executor avoids shadowing other
data that might have been injected previously, e.g., a check update includes the
last known update from a health check.

It is the responsibility of the executor to interpret `CheckInfo` and
`HealthCheckInfo` and perform checks appropriately. All built-in executors
support health checking their tasks and all except the docker executor support
generalized checks (see [implementation details](#under-the-hood) and
[limitations](#current-limitations)).

**NOTE:** It is up to the executor how---and whether at all---to honor the
`CheckInfo` and `HealthCheck` fields in `TaskInfo`. Implementations may vary
significantly depending on what entity `TaskInfo` represents. On this page only
the reference implementation for built-in executors is considered.

Custom executors can use [the checker library](#under-the-hood), the reference
implementation for health checking that all built-in executors rely on.


### On the Differences Between Checks and Health Checks
When humans read data from a sensor, they may interpret these data and act on
them. For example, if they check air temperature, they usually interpret
temperature readings and say whether it's cold or warm outside; they may also
act on the interpretation and decide to apply sunscreen or put on an extra
jacket.

Similar reasoning can be applied to checking task's state in Mesos:

1. Perform a check.
2. Optionally interpret the result and, for example, declare the task either
   healthy or unhealthy.
3. Optionally act on the interpretation by killing an unhealthy task.

Mesos health checks do all of the above, 1+2+3: they run the check, declare the
task healthy or not, and kill it after `consecutive_failures` have occurred.
Though efficient and scalable, this strategy is inflexible for the needs of
frameworks which may want to run an arbitrary check without Mesos interpreting
the result in any way, for example, to transmit the task's internal state
transitions and make global decisions.

Conceptually, a health check is a check with an interpretation and a kill
policy. A check and a health check differ in how they are specified and
implemented:

* Built-in executors do not (and custom executors shall not) interpret the
  result of a check. If they do, it should be a health check.
* There is no concept of a check failure, hence grace period and consecutive
  failures options are only available for health checks. Note that a check can
  still time out (a health check interprets timeouts as failures), in this case
  an empty result is sent to the scheduler.
* Health checks do not propagate the result of the underlying check to the
  scheduler, only its interpretation: healthy or unhealthy. Note that this may
  change in the future.
* Health check updates are deduplicated based on the interpretation and not the
  result of the underlying check, i.e., given that only HTTP `4**` status codes
  are considered failures, if the first HTTP check returns `200` and the second
  `202`, only one status update after the first success is sent, while a check
  would generate two status updates in this case.

**NOTE:** Docker executor currently supports health checks but not checks.

**NOTE:** Slight changes in protobuf message naming and structure are due to
backward compatibility reasons; in the future the `HealthCheck` message will be
based on `CheckInfo`.


<a name="anatomy-of-a-check"></a>
## Anatomy of a Check

A `CheckStatusInfo` message is added to the task status update to convey the
check status. Currently, check status info is only added for `TASK_RUNNING`
status updates.

Built-in executors leverage task status updates to deliver check updates to the
scheduler. To minimize performance overhead, a check-related task status update
is triggered if and only if the value or presence of any field in
`CheckStatusInfo` changes. As the `CheckStatusInfo` message matures, in the
future we might deduplicate only on specific fields in `CheckStatusInfo` to make
sure that as few updates as possible are sent. Note that custom executors may
use a different strategy.

To support third party tooling that might not have access to the original
`TaskInfo` specification, `TaskStatus.check_status` generated by built-in
executors adheres to the following conventions:

* If the original `TaskInfo` has not specified a check,
  `TaskStatus.check_status` is not present.
* If the check has been specified, `TaskStatus.check_status.type` indicates the
  check's type.
* If the check result is not available for some reason (a check has not run yet
  or a check has timed out), the corresponding result is empty, e.g.,
  `TaskStatus.check_status.command` is present and empty.

**NOTE:** Frameworks that use custom executors are highly advised to follow the
same principles built-in executors use for consistency.


<a name="command-checks"></a>
### Command Checks

Command checks are described by the `CommandInfo` protobuf wrapped in the
`CheckInfo.Command` message; some fields are ignored though: `CommandInfo.user`
and `CommandInfo.uris`. A command check specifies an arbitrary command that is
used to check a particular condition of the task. The result of the check is the
exit code of the command.

**NOTE:** Docker executor does not currently support checks. For all other
tasks, including Docker containers launched in the
[mesos containerizer](mesos-containerizer.md), the command will be executed from
the task's mount namespace.

To specify a command check, set `type` to `CheckInfo::COMMAND` and populate
`CheckInfo.Command.CommandInfo`, for example:

~~~{.cpp}
TaskInfo task = [...];

CheckInfo check;
check.set_type(CheckInfo::COMMAND);
check.mutable_command()->mutable_command()->set_value(
    "ls /checkfile > /dev/null");

task.mutable_check()->CopyFrom(check);
~~~


<a name="http-checks"></a>
### HTTP Checks

HTTP checks are described by the `CheckInfo.Http` protobuf with `port` and
`path` fields. A `GET` request is sent to `http://<host>:port/path` using the
`curl` command. Note that `<host>` is currently not configurable and is set
automatically to `127.0.0.1` (see [limitations](#current-limitations)), hence
the checked task must listen on the loopback interface along with any other
routeable interface it might be listening on. Field `port` must specify an
actual port the task is listening on, not a mapped one. The result of the check
is the HTTP status code of the response.

Built-in executors follow HTTP `3xx` redirects; custom executors may employ a
different strategy.

If necessary, executors enter the task's network namespace prior to launching
the `curl` command.

**NOTE:** HTTPS checks are currently not supported.

To specify an HTTP check, set `type` to `CheckInfo::HTTP` and populate
`CheckInfo.Http`, for example:

~~~{.cpp}
TaskInfo task = [...];

CheckInfo check;
check.set_type(CheckInfo::HTTP);
check.mutable_http()->set_port(8080);
check.mutable_http()->set_path("/health");

task.mutable_check()->CopyFrom(check);
~~~


<a name="tcp-checks"></a>
### TCP Checks

TCP checks are described by the `CheckInfo.Tcp` protobuf, which has a single
`port` field, which must specify an actual port the task is listening on, not a
mapped one. The task is probed using Mesos' `mesos-tcp-connect` command, which
tries to establish a TCP connection to `<host>:port`. Note that `<host>` is
currently not configurable and is set automatically to `127.0.0.1`
(see [limitations](#current-limitations)), hence the checked task must listen on
the loopback interface along with any other routeable interface it might be
listening on. Field `port` must specify an actual port the task is listening on,
not a mapped one. The result of the check is the boolean value indicating
whether a TCP connection succeeded.

If necessary, executors enter the task's network namespace prior to launching
the `mesos-tcp-connect` command.

To specify a TCP check, set `type` to `CheckInfo::TCP` and populate
`CheckInfo.Tcp`, for example:

~~~{.cpp}
TaskInfo task = [...];

CheckInfo check;
check.set_type(CheckInfo::TCP);
check.mutable_tcp()->set_port(8080);

task.mutable_check()->CopyFrom(check);
~~~


### Common options

The `CheckInfo` protobuf contains common options which regulate how a check must
be performed by an executor:

* `delay_seconds` is the amount of time to wait until starting checking the
  task.
* `interval_seconds` is the interval between check attempts.
* `timeout_seconds` is the amount of time to wait for the check to complete.
  After this timeout, the check attempt is aborted and empty check update,
  i.e., the absence of the check result, is reported.

**NOTE:** Since each time a check is performed a helper command is launched
(see [limitations](#current-limitations)), setting `timeout_seconds` to a small
value, e.g., `<5s`, may lead to intermittent failures.

**NOTE:** Launching a check is not a free operation. To avoid unpredictable
spikes in agent's load, e.g., when most of the tasks run their checks
simultaneously, avoid setting `interval_seconds` to zero.

As an example, the code below specifies a task which is a Docker container with
a simple HTTP server listening on port `8080` and an HTTP check that should be
performed every `5` seconds starting from the task launch and response time
under `1` second.

~~~{.cpp}
TaskInfo task = createTask(...);

// Use Netcat to emulate an HTTP server.
const string command =
    "nc -lk -p 8080 -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"";
task.mutable_command()->set_value(command)

Image image;
image.set_type(Image::DOCKER);
image.mutable_docker()->set_name("alpine");

ContainerInfo* container = task.mutable_container();
container->set_type(ContainerInfo::MESOS);
container->mutable_mesos()->mutable_image()->CopyFrom(image);

// Set `delay_seconds` here because it takes
// some time to launch Netcat to serve requests.
CheckInfo check;
check.set_type(CheckInfo::HTTP);
check.mutable_http()->set_port(8080);
check.set_delay_seconds(15);
check.set_interval_seconds(5);
check.set_timeout_seconds(1);

task.mutable_check()->CopyFrom(check);
~~~


## Anatomy of a Health Check

The boolean `healthy` field is used to convey health status, which
[may be insufficient](#current-limitations) in certain cases. This means a task
that has failed health checks will be `RUNNING` with `healthy` set to `false`.
Currently, the `healthy` field is only set for `TASK_RUNNING` status updates.

When a task turns unhealthy, a task status update message with the `healthy`
field set to `false` is sent to the Mesos master and then forwarded to a
scheduler. The executor is expected to kill the task after a number of
consecutive failures defined in the `consecutive_failures` field of the
`HealthCheck` protobuf.

**NOTE:** While a scheduler currently cannot cancel a task kill due to failing
health checks, it may issue a `killTask` command itself. This may be helpful to
emulate a "global" policy for handling tasks with failing health checks (see
[limitations](#current-limitations)). Alternatively, the scheduler might use
[generalized checks](#anatomy-of-a-check) instead.

Built-in executors forward all unhealthy status updates, as well as the first
healthy update when a task turns healthy, i.e., when the task has started or
after one or more unhealthy updates have occurred. Note that custom executors
may use a different strategy.


<a name="command-health-checks"></a>
### Command Health Checks

Command health checks are described by the `CommandInfo` protobuf; some fields
are ignored though: `CommandInfo.user` and `CommandInfo.uris`. A command health
check specifies an arbitrary command that is used to validate the health of the
task. The executor launches the command and inspects its exit status: `0` is
treated as success, any other status as failure.

**NOTE:** If a task is a Docker container launched by the docker executor, it
will be wrapped in `docker run`. For all other tasks, including Docker
containers launched in the [mesos containerizer](mesos-containerizer.md), the
command will be executed from the task's mount namespace.

To specify a command health check, set `type` to `HealthCheck::COMMAND` and
populate `CommandInfo`, for example:

~~~{.cpp}
TaskInfo task = [...];

HealthCheck healthCheck;
healthCheck.set_type(HealthCheck::COMMAND);
healthCheck.mutable_command()->set_value("ls /checkfile > /dev/null");

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


<a name="http-health-checks"></a>
### HTTP(S) Health Checks

HTTP(S) health checks are described by the `HealthCheck.HTTPCheckInfo` protobuf
with `scheme`, `port`, `path`, and `statuses` fields. A `GET` request is sent to
`scheme://<host>:port/path` using the `curl` command. Note that `<host>` is
currently not configurable and is set automatically to `127.0.0.1` (see
[limitations](#current-limitations)), hence the health checked task must listen
on the loopback interface along with any other routeable interface it might be
listening on. The `scheme` field supports `"http"` and `"https"` values only.
Field `port` must specify an actual port the task is listening on, not a mapped
one.

Built-in executors follow HTTP `3xx` redirects and treat status codes between
`200` and `399` as success; custom executors may employ a different strategy,
e.g., leveraging the `statuses` field.

**NOTE:** Setting `HealthCheck.HTTPCheckInfo.statuses` has no effect on the
built-in executors.

If necessary, executors enter the task's network namespace prior to launching
the `curl` command.

To specify an HTTP health check, set `type` to `HealthCheck::HTTP` and populate
`HTTPCheckInfo`, for example:

~~~{.cpp}
TaskInfo task = [...];

HealthCheck healthCheck;
healthCheck.set_type(HealthCheck::HTTP);
healthCheck.mutable_http()->set_port(8080);
healthCheck.mutable_http()->set_scheme("http");
healthCheck.mutable_http()->set_path("/health");

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


<a name="tcp-health-checks"></a>
### TCP Health Checks

TCP health checks are described by the `HealthCheck.TCPCheckInfo` protobuf,
which has a single `port` field, which must specify an actual port the task is
listening on, not a mapped one. The task is probed using Mesos'
`mesos-tcp-connect` command, which tries to establish a TCP connection to
`<host>:port`. Note that `<host>` is currently not configurable and is set
automatically to `127.0.0.1` (see [limitations](#current-limitations)), hence
the health checked task must listen on the loopback interface along with any
other routeable interface it might be listening on. Field `port` must specify an
actual port the task is listening on, not a mapped one.

The health check is considered successful if the connection can be established.

If necessary, executors enter the task's network namespace prior to launching
the `mesos-tcp-connect` command.

To specify a TCP health check, set `type` to `HealthCheck::TCP` and populate
`TCPCheckInfo`, for example:

~~~{.cpp}
TaskInfo task = [...];

HealthCheck healthCheck;
healthCheck.set_type(HealthCheck::TCP);
healthCheck.mutable_tcp()->set_port(8080);

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


### Common options

The `HealthCheck` protobuf contains common options which regulate how a health
check must be performed and interpreted by an executor:

* `delay_seconds` is the amount of time to wait until starting health checking
  the task.
* `interval_seconds` is the interval between health checks.
* `timeout_seconds` is the amount of time to wait for the health check to
  complete. After this timeout, the health check is aborted and treated as a
  failure.
* `consecutive_failures` is the number of consecutive failures until the task is
  killed by the executor.
* `grace_period_seconds` is the amount of time after the task is launched during
  which health check failures are ignored. Once a health check succeeds for the
  first time, the grace period does not apply anymore. Note that it includes
  `delay_seconds`, i.e., setting `grace_period_seconds` < `delay_seconds` has
  no effect.

**NOTE:** Since each time a health check is performed a helper command is
launched (see [limitations](#current-limitations)), setting `timeout_seconds`
to a small value, e.g., `<5s`, may lead to intermittent failures.

As an example, the code below specifies a task which is a Docker container with
a simple HTTP server listening on port `8080` and an HTTP health check that
should be performed every `5` seconds starting from the task launch and allows
consecutive failures during the first `15` seconds and response time under `1`
second.

~~~{.cpp}
TaskInfo task = createTask(...);

// Use Netcat to emulate an HTTP server.
const string command =
    "nc -lk -p 8080 -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"";
task.mutable_command()->set_value(command)

Image image;
image.set_type(Image::DOCKER);
image.mutable_docker()->set_name("alpine");

ContainerInfo* container = task.mutable_container();
container->set_type(ContainerInfo::MESOS);
container->mutable_mesos()->mutable_image()->CopyFrom(image);

// Set `grace_period_seconds` here because it takes
// some time to launch Netcat to serve requests.
HealthCheck healthCheck;
healthCheck.set_type(HealthCheck::HTTP);
healthCheck.mutable_http()->set_port(8080);
healthCheck.set_delay_seconds(0);
healthCheck.set_interval_seconds(5);
healthCheck.set_timeout_seconds(1);
healthCheck.set_grace_period_seconds(15);

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


<a name="under-the-hood"></a>
## Under the Hood

All built-in executors rely on the checker library, which lives in
["src/checks"](https://github.com/apache/mesos/tree/master/src/checks).
An executor creates an instance of the `Checker` or `HealthChecker` class per
task and passes the check or health check definition together with extra
parameters. In return, the library notifies the executor of changes in the
task's check or health status. For health checks, the definition is converted
to the check definition before performing the check, and the check result is
interpreted according to the health check definition.

The library depends on `curl` for HTTP(S) checks and `mesos-tcp-connect` for
TCP checks (the latter is a simple command bundled with Mesos).

One of the most non-trivial things the library takes care of is entering the
appropriate task's namespaces (`mnt`, `net`) on Linux agents. To perform a
command check, the checker must be in the same mount namespace as the checked
process; this is achieved by either calling `docker run` for the check command
in case of [docker containerizer](docker-containerizer.md) or by explicitly
calling `setns()` for `mnt` namespace in case of [mesos containerizer](mesos-containerizer.md)
(see [containerization in Mesos](containerizers.md)). To perform an HTTP(S) or
TCP check, the most reliable solution is to share the same network namespace
with the checked process; in case of docker containerizer `setns()` for `net`
namespace is explicitly called, while mesos containerizer guarantees an executor
and its tasks are in the same network namespace.

**NOTE:** Custom executors may or may not use this library. Please consult the
respective framework's documentation.

Regardless of executor, all checks and health checks consume resources from the
task's resource allocation. Hence it is a good idea to add some extra resources,
e.g., 0.05 cpu and 32MB mem, to the task definition if a Mesos-native check
and/or health check is specified.

**Windows Implementation**

On Windows, the implementation differs between the [mesos containerizer](mesos-containerizer.md)
and [docker containerizer](docker-containerizer.md). The
[mesos containerizer](mesos-containerizer.md) does not provide network or mount
namespace isolation, so `curl`, `mesos-tcp-connect` or the command health check
simply run as regular processes on the host. In constrast, the
[docker containerizer](docker-containerizer.md) provides network and mount
isolation. For the command health check, the command enters the container's
namespace through `docker exec`. For the network health checks, the docker
executor launches a container with the [`mesos/windows-health-check`](https://hub.docker.com/r/mesos/windows-health-check)
image and enters the original container's network namespace through the
`--network=container:<ID>` parameter in `docker run`.

<a name="current-limitations"></a>
## Current Limitations and Caveats

* Docker executor does not support generalized checks (see
  [MESOS-7250](https://issues.apache.org/jira/browse/MESOS-7250)).
* HTTPS checks are not supported, though HTTPS health checks are (see
  [MESOS-7356](https://issues.apache.org/jira/browse/MESOS-7356)).
* Due to the short-polling nature of a check, some task state transitions may be
  missed. For example, if the task transitions are `Init [111]` &rarr;
  `Join [418]` &rarr; `Ready [200]`, the observed HTTP status codes in check
  statuses may be `111` &rarr; `200`.
* Due to its short-polling nature, a check whose state oscillates repeatedly
  may lead to scalability issues due to a high volume of task status updates.
* When a task becomes unhealthy, it is deemed to be killed after
  `HealthCheck.consecutive_failures` failures. This decision is taken locally by
  an executor, there is no way for a scheduler to intervene and react
  differently. A workaround is to set `HealthCheck.consecutive_failures` to some
  large value so that the scheduler can react. One possible solution is to
  introduce a "global" policy for handling unhealthy tasks (see
  [MESOS-6171](https://issues.apache.org/jira/browse/MESOS-6171)).
* HTTP(S) and TCP health checks use `127.0.0.1` as target IP. As a result, if
  tasks want to support HTTP or TCP health checks, they should listen on the
  loopback interface in addition to whatever interface they require (see
  [MESOS-6517](https://issues.apache.org/jira/browse/MESOS-6517)).
* HTTP(S) health checks rely on the `curl` command. A health check is
  considered failed if the required command is not available.
* Windows HTTP(S) and TCP Docker health checks should ideally have the
  `mesos/windows-health-check` image pulled beforehand. Otherwise, Docker will
  attempt to pull the image during the health check, which will count towards
  the health check timeout.
* Only a single health check per task is allowed (see
  [MESOS-5962](https://issues.apache.org/jira/browse/MESOS-5962)).
* Each time a health check runs, [a helper command is launched](#under-the-hood).
  This introduces some run-time overhead (see
  [MESOS-6766](https://issues.apache.org/jira/browse/MESOS-6766)).
* A task without a health check may be indistinguishable from a task with a
  health check but still in a grace period. An extra state should be introduced
  (see [MESOS-6417](https://issues.apache.org/jira/browse/MESOS-6417)).
* Task's health status cannot be assigned from outside, e.g., by an operator via
  an endpoint.
