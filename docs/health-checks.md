---
title: Apache Mesos - Task Health Checking
layout: documentation
---

# Task Health Checking

Sometimes applications crash, misbehave, or become unresponsive. To detect and
recover from such situations, some frameworks (e.g.,
[Marathon](https://github.com/mesosphere/marathon/blob/v1.3.6/docs/docs/health-checks.md),
[Apache Aurora](https://aurora.apache.org/documentation/0.8.0/user-guide/#http-health-checking-and-graceful-shutdown))
implement their own logic for checking the health of their tasks. This is
typically done by having the framework scheduler send a "ping" request, e.g.,
via HTTP, to the host where the task is running and arranging for the task or
executor to respond to the ping. Although this technique is extremely useful,
there are several disadvantages in the way it is usually implemented:

* Each Mesos framework uses its own API and protocol.
* Framework developers have to reimplement common functionality.
* Health checks originating from a scheduler generate extra network traffic if
  the task and the scheduler run on different nodes (which is usually the case);
  moreover, network failures between the task and the scheduler may make the
  latter think that the former is unhealthy, which might not be the case.
* Implementing health checks in the framework scheduler can be a performance
  bottleneck. If a framework is managing a large number of tasks, performing
  health checks for every task can cause scheduler performance problems.

To address the aforementioned problems, Mesos 1.2.0 introduces
[the Mesos-native health check design](#mesos-native-health-checks), defines
common API for [command](#command-health-checks), [HTTP(S)](#http-health-checks),
and [TCP](#tcp-health-checks) health checks, and provides reference
implementations for all built-in executors.

**NOTE:** Some functionality related to health checking was available prior to
1.2.0 release, however it was considered experimental.

**NOTE:** Mesos monitors each process-based task, including Docker containers,
using an equivalent of a `waitpid()` system call. This technique allows
detecting and reporting process crashes, but is insufficient for cases when the
process is still running but is not responsive.

This document describes supported health check types, touches on relevant
implementation details, and mentions limitations and caveats.


<a name="mesos-native-health-checks"></a>
## Mesos-native Health Checks

In contrast to the state-of-the-art "scheduler health check" pattern mentioned
above, Mesos-native health checks run on the agent node: it is the executor
which performs checks and not the scheduler. This improves scalability but means
that detecting network faults or task availability from the outside world
becomes a separate concern. For instance, if the task is running on a
partitioned agent, it will still be health checked and---if those health checks
fail---will be terminated. Needless to say that due to the network partition,
all this will happen without the framework scheduler being notified.

Task status updates are leveraged to transfer the health check status to the
Mesos master and further to the framework's scheduler ensuring the
"at-least-once" delivery guarantee. The boolean `healthy` field is used to
convey health status, which [may be insufficient](#current-limitations) in
certain cases. This means a task that has failed health checks will be `RUNNING`
with `healthy` set to `false`. Currently, the `healthy` field is only set for
`TASK_RUNNING` status updates.

When a task turns unhealthy, a task status update message with the `healthy`
field set to `false` is sent to the Mesos master and then forwarded to a
scheduler. The executor is expected to kill the task after a number of
consecutive failures defined in the `consecutive_failures` field of the
`HealthCheck` protobuf.

**NOTE:** While a scheduler currently cannot cancel a task kill due to failing
health checks, it may issue a `killTask` command itself. This may be helpful to
emulate a "global" policy for handling tasks with failing health checks (see
[limitations](#current-limitations)).

Built-in executors forward all unhealthy status updates, as well as the first
healthy update when a task turns healthy, i.e., when the task has started or
after one or more unhealthy updates have occurred. Note that custom executors
may use a different strategy.

Custom executors can use [the health checker library](#under-the-hood), the
reference implementation for health checking all built-in executors rely on.


## Anatomy of a Health Check

Mesos health checks are described in the
[`HealthCheck`](https://github.com/apache/mesos/blob/1.1.0/include/mesos/mesos.proto#L345)
protobuf. Currently, only tasks can be health checked, not arbitrary processes
or executors, i.e., only the `TaskInfo` protobuf has the optional `HealthCheck`
field. However, it is worth noting that all built-in executors map a task to a
process.

It is an executor's responsibility to health check its tasks, because only
executor knows how to interpret `TaskInfo`. All built-in executors support
health checking their tasks (see [implementation details](#under-the-hood)
and [limitations](#current-limitations)).

**NOTE:** It is up to the executor how---and whether at all---to honor
the `HealthCheck` field in `TaskInfo`. Implementations may vary significantly
depending on what entity `TaskInfo` represents. In this section only the
reference implementation for built-in executors is considered.


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
currently not configurable and is resolved automatically to `127.0.0.1` (see
[limitations](#current-limitations)). The `scheme` field supports `"http"` and
`"https"` values only. Field `port` must specify an actual port the task is
listening on, not a mapped one.

Built-in executors treat status codes between `200` and `399` as success; custom
executors may employ a different strategy, e.g., leveraging the `statuses` field.

**NOTE:** Setting `HealthCheck.HTTPCheckInfo.statuses` has no effect on the
built-in executors.

If necessary, executors enter the task's network namespace prior to launching
the `curl` command.

To specify an HTTP health check, set `type` to `HealthCheck::HTTP` and populate
`HTTPCheckInfo`, for example:

~~~{.cpp}
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
`<host>:port`. Note that `<host>` is currently not configurable and is resolved
automatically to `127.0.0.1` (see [limitations](#current-limitations)).

The health check is considered successful if the connection can be established.

If necessary, executors enter the task's network namespace prior to launching
the `mesos-tcp-connect` command.

To specify a TCP health check, set `type` to `HealthCheck::TCP` and populate
`TCPCheckInfo`, for example:

~~~{.cpp}
HealthCheck healthCheck;
healthCheck.set_type(HealthCheck::TCP);
healthCheck.mutable_tcp()->set_port(8080);

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


### Common options

The `HealthCheck` protobuf contains common options which regulate how a health
check must be interpreted by an executor:

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
should be performed every second starting from the task launch and allows
consecutive failures during first `15` seconds and response time under `1`
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
healthCheck.set_interval_seconds(1);
healthCheck.set_timeout_seconds(1);
healthCheck.set_grace_period_seconds(15);

task.mutable_health_check()->CopyFrom(healthCheck);
~~~


<a name="under-the-hood"></a>
## Under the Hood

All built-in executors rely on the health checker library, which lives in
["src/checks"](https://github.com/apache/mesos/tree/master/src/checks).
An executor creates an instance of the `HealthChecker` per task and passes the
health check definition together with extra parameters. In return, the library
notifies the executor of changes in the task's health status.

The library depends on `curl` for HTTP(S) checks and `mesos-tcp-connect` for
TCP checks (the latter is a simple command bundled with Mesos).

One of the most non-trivial things the library takes care of is entering the
appropriate task's namespaces (`mnt`, `net`) on Linux agents. To perform a
command health check, the checker must be in the same mount namespace as the
checked process; this is achieved by either calling `docker run` for the health
check command in case of [docker containerizer](docker-containerizer.md) or
by explicitly calling `setns()` for `mnt` namespace in case of
[mesos containerizer](mesos-containerizer.md) (see
[containerization in Mesos](containerizers.md)). To perform an HTTP(S) or TCP
health check, the most reliable solution is to share the same network namespace
with the checked process; in case of docker containerizer `setns()` for `net`
namespace is explicitly called, while mesos containerizer guarantees an executor
and its tasks are in the same network namespace.

**NOTE:** Custom executors may or may not use this library. Please check the
respective framework's documentation.

Regardless of executor, all resources used to health check a task are accounted
towards task's resource allocation. Hence it is a good idea to add some extra
resources, e.g., 0.05 cpu and 32MB mem, to the task definition if a Mesos-native
health check is specified.


<a name="current-limitations"></a>
## Current Limitations

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
* HTTP(S) health checks rely on the `curl` command; if it is not available, a
  health check is considered failed.
* TCP health checks are not supported on Windows (see
  [MESOS-6117](https://issues.apache.org/jira/browse/MESOS-6117)).
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
