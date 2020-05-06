---
title: Apache Mesos - Mesos Nested Container and Task Group
layout: documentation
---

# Overview


## Motivation

A [pod](http://kubernetes.io/docs/user-guide/pods) can be defined as
a set of containers co-located and co-managed on an agent that share
some resources (e.g., network namespace, volumes) but not others
(e.g., container image, resource limits). Here are the use cases for
pod:

* Run a side-car container (e.g., logger, backup) next to the main
  application controller.
* Run an adapter container (e.g., metrics endpoint, queue consumer)
  next to the main container.
* Run transient tasks inside a pod for operations which are
  short-lived and whose exit does not imply that a pod should
  exit (e.g., a task which backs up data in a persistent volume).
* Provide performance isolation between latency-critical application
  and supporting processes.
* Run a group of containers sharing volumes and network namespace
  while some of them can have their own mount namespace.
* Run a group of containers with the same life cycle, e.g, one
  container's failure would cause all other containers being
  cleaned up.

In order to have first class support for running "pods", two new
primitives are introduced in Mesos: `Task Group` and `Nested Container`.


## Background

Mesos has the concept of Executors and Tasks. An executor can launch
multiple tasks while the executor runs in a container. An agent can
run multiple executors. The pod can be implemented by leveraging the
executor and task abstractions. More specifically, the executor runs
in the top level container (called executor container) and its tasks
run in separate nested containers inside this top level container,
while the container image can be specified for each container.


## Task Groups

The concept of a "task group" addresses a previous limitation of the scheduler
and executor APIs, which could not send a group of tasks to an executor
atomically. Even though a scheduler can launch multiple tasks for the same
executor in a LAUNCH operation, these tasks are delivered to the executor one at
a time via separate LAUNCH events. It cannot guarantee atomicity since any
individual task might be dropped due to different reasons (e.g., network
partition). Therefore, the task group provides all-or-nothing semantics to
ensure a group of tasks are delivered atomically to an executor.


## Nested Containers

The concept of a "nested container" describes containers nested under an
executor container. In the typical case of a Linux agent, they share a network
namespace and volumes so that they can communicate using the network and access
the same data, though they may have their own container images and resource
limits. On Linux, they may share cgroups or have their own - see the section
below on resource limits for more information.

With the agent nested container API, executors can utilize the
containerizer in the agent to launch nested containers. Both authorized
operators or executors will be allowed to create nested containers. The Mesos
default executor makes use of this API when launching tasks, and custom
executors may consume it as well.


## Resource Requests and Limits

In each task, the resources required by that task can be specified. Common
resource types are `cpus`, `mem`, and `disk`. The resources listed in the
`resources` field are known as resource "requests" and represent the minimum
resource guarantee required by the task; these resources are used to set the
cgroups of the nested container associated with the task and will always be
available to the task process if they are needed. The quantities specified in
the `limits` field are the resource "limits", which represent the maximum amount
of `cpus` and/or `mem` that the task may use. Setting a CPU or memory limit
higher than the corresponding request allows the task to consume more than its
allocated amount of CPU or memory when there are unused resources available on
the agent.

When multiple nested containers run under a single executor, the enforcement
of resource constraints depends on the value of the
`container.linux_info.share_cgroups` field. When this boolean field is `true`
(this is the default), each container is constrained by the cgroups of its
parent container. This means that if multiple tasks run underneath one executor,
their resource constraints will be enforced as a sum of all the task resource
constraints, applied collectively to those task processes. In this case, nested
container resource consumption is collectively managed via one set of cgroup
subsystem control files associated with the parent executor container.

When the `share_cgroups` field is set to `false`, the resource consumption of
each task is managed via a unique set of cgroups associated with that task's
nested container, which means that each task process is subject to its own
resource requests and limits. Note that if you want to specify `limits` on a
task, the task's container MUST set `share_cgroups` to `false`. Also note that
all nested containers under a single executor container must share the same
value of `share_cgroups`.

Note that when a task sets a memory limit higher than its memory request, the
Mesos agent will change the OOM score adjustment of the task process using a
heuristic based on the task's memory request and the agent's memory capacity.
This means that if the agent's memory becomes exhausted and processes must be
OOM-killed to reclaim memory at a time when the task is consuming more than its
memory request, the task process will be killed preferentially.


# Task Group API


## Framework API

    message TaskGroupInfo {
      repeated TaskInfo tasks = 1;
    }

    message Offer {
      ...

      message Operation {
        enum Type {
          ...
          LAUNCH_GROUP = 6;
          ...
        }
        ...

        message LaunchGroup {
          required ExecutorInfo executor = 1;
          required TaskGroupInfo task_group = 2;
        }
        ...

        optional LaunchGroup launch_group = 7;
      }
    }

By using the TaskGroup Framework API, frameworks can launch a task
group with the [default executor](app-framework-development-guide.md)
or a custom executor. The group of tasks can be specified through an
offer operation `LaunchGroup` when accepting an offer. The
`ExecutorInfo` indicates the executor to launch the task group, while
the `TaskGroupInfo` includes the group of tasks to be launched
atomically.


To use the default executor for launching the task group, the framework should:

* Set `ExecutorInfo.type` as `DEFAULT`.
* Set `ExecutorInfo.resources` for the resources needed for the executor.

Please note that the following fields in the `ExecutorInfo` are not allowed to set when using the default executor:

* `ExecutorInfo.command`.
* `ExecutorInfo.container.type`, `ExecutorInfo.container.docker` and `ExecutorInfo.container.mesos`.

To allow containers to share a network namespace:

* Set `ExecutorInfo.container.network`.

To allow containers to share an ephemeral volume:

* Specify the `volume/sandbox_path` isolator.
* Set `TaskGroupInfo.tasks.container.volumes.source.type` as `SANDBOX_PATH`.
* Set `TaskGroupInfo.tasks.container.volumes.source.sandbox_path.type` as `PARENT` and the path relative to the parent container's sandbox.

## Executor API

    message Event {
      enum Type {
        ...
        LAUNCH_GROUP = 8;
        ...
      }
      ...

      message LaunchGroup {
        required TaskGroupInfo task_group = 1;
      }
      ...

      optional LaunchGroup launch_group = 8;
    }

A new event `LAUNCH_GROUP` is added to Executor API. Similar to the
Framework API, the `LAUNCH_GROUP` event guarantees a group of tasks
are delivered to the executor atomically.

# Nested Container API


## New Agent API

    package mesos.agent;

    message Call {
      enum Type {
        ...
        // Calls for managing nested containers underneath an executor's container.
        NESTED_CONTAINER_LAUNCH = 14;  // See 'NestedContainerLaunch' below.
        NESTED_CONTAINER_WAIT = 15;    // See 'NestedContainerWait' below.
        NESTED_CONTAINER_KILL = 16;    // See 'NestedContainerKill' below.
      }

      // Launches a nested container within an executor's tree of containers.
      message LaunchNestedContainer {
        required ContainerID container_id = 1;
        optional CommandInfo command = 2;
        optional ContainerInfo container = 3;
      }

      // Waits for the nested container to terminate and receives the exit status.
      message WaitNestedContainer {
        required ContainerID container_id = 1;
      }

      // Kills the nested container. Currently only supports SIGKILL.
      message KillNestedContainer {
        required ContainerID container_id = 1;
      }

      optional Type type = 1;
      ...
      optional NestedContainerLaunch nested_container_launch = 6;
      optional NestedContainerWait nested_container_wait = 7;
      optional NestedContainerKill nested_container_kill = 8;
    }

    message Response {
      enum Type {
        ...
        NESTED_CONTAINER_WAIT = 13;    // See 'NestedContainerWait' below.
      }

      // Returns termination information about the nested container.
      message NestedContainerWait {
        optional int32 exit_status = 1;
      }

      optional Type type = 1;
      ...
      optional NestedContainerWait nested_container_wait = 14;
    }

By adding the new Agent API, any authorized entity, including the
executor itself, its tasks, or the operator can use this API to
launch/wait/kill nested containers. Multi-level nesting is supported
by using this API. Technically, the nested level is up to 32 since
it is limited by the maximum depth of [pid namespace](https://github.com/torvalds/linux/commit/f2302505775fd13ba93f034206f1e2a587017929) and
[user namespace](http://man7.org/linux/man-pages/man7/user_namespaces.7.html) from the Linux Kernel.

The following is the workflow of how the new Agent API works:

1. The executor sends a `NESTED_CONTAINER_LAUNCH` call to the agent.

                                       +---------------------+
                                       |                     |
                                       |     Container       |
                                       |                     |
        +-------------+                | +-----------------+ |
        |             |     LAUNCH     | |                 | |
        |             | <------------+ | |    Executor     | |
        |             |                | |                 | |
        | Mesos Agent |                | +-----------------+ |
        |             |                |                     |
        |             |                |                     |
        |             |                |                     |
        +-------------+                |                     |
                                       +---------------------+

2. Depending on the `LaunchNestedContainer` from the executor, the
agent launches a nested container inside of the executor container
by calling `containerizer::launch()`.

                                       +---------------------+
                                       |                     |
                                       |     Container       |
                                       |                     |
        +-------------+                | +-----------------+ |
        |             |     LAUNCH     | |                 | |
        |             | <------------+ | |    Executor     | |
        |             |                | |                 | |
        | Mesos Agent |                | +-----------------+ |
        |             |                |                     |
        |             |                | +---------+         |
        |             | +------------> | |Nested   |         |
        +-------------+                | |Container|         |
                                       | +---------+         |
                                       +---------------------+

3. The executor sends a `NESTED_CONTAINER_WAIT` call to the agent.

                                       +---------------------+
                                       |                     |
                                       |     Container       |
                                       |                     |
        +-------------+                | +-----------------+ |
        |             |      WAIT      | |                 | |
        |             | <------------+ | |    Executor     | |
        |             |                | |                 | |
        | Mesos Agent |                | +-----------------+ |
        |             |                |                     |
        |             |                | +---------+         |
        |             |                | |Nested   |         |
        +-------------+                | |Container|         |
                                       | +---------+         |
                                       +---------------------+

4. Depending on the `ContainerID`, the agent calls `containerizer::wait()`
to wait for the nested container to terminate or exit. Once the container
terminates or exits, the agent returns the container exit status to the
executor.

                                       +---------------------+
                                       |                     |
                                       |     Container       |
                                       |                     |
        +-------------+      WAIT      | +-----------------+ |
        |             | <------------+ | |                 | |
        |             |                | |    Executor     | |
        |             | +------------> | |                 | |
        | Mesos Agent |  Exited with   | +-----------------+ |
        |             |  status 0      |                     |
        |             |                | +--XX-XX--+         |
        |             |                | |   XXX   |         |
        +-------------+                | |   XXX   |         |
                                       | +--XX-XX--+         |
                                       +---------------------+

# Future Work

* Authentication and authorization on the new Agent API.
* Command health checks inside of the container's mount namespace.
* Resource isolation for nested containers.
* Resource statistics reporting for nested containers.
* Multiple task groups.

# Reference

* [Kubernetes Pods](http://kubernetes.io/docs/user-guide/pods/)
* [Docker Pods](https://github.com/docker/docker/issues/8781)
