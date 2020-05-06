---
title: Apache Mesos - Running Workloads in Mesos
layout: documentation
---

# Workloads in Mesos

The goal of most Mesos schedulers is to launch workloads on Mesos agents. Once
a scheduler has subscribed with the Mesos master using the
[`SUBSCRIBE` call](scheduler-http-api.md#subscribe), it will begin to receive
offers. To launch a workload, the scheduler can submit an
[`ACCEPT` call](scheduler-http-api.md#accept) to the master, including the offer
ID of an offer that it previously received which contains resources it can use
to run the workload.

The basic unit of work in a Mesos cluster is the "task". A single command or
container image and accompanying artifacts can be packaged into a task which is
sent to a Mesos agent for execution. To launch a task, a scheduler can place it
into a task group and pass it to the Mesos master inside a `LAUNCH_GROUP`
operation. `LAUNCH_GROUP` is one of the offer operations that can be specified
in the `ACCEPT` call.

An older call in the same API, the `LAUNCH` call, allows schedulers to launch
single tasks as well; this legacy method of launching tasks will be covered at
the end of this document.

# Task Groups

Task groups, or "pods", allow a scheduler to group one or more tasks into a
single workload. When one task is specified alongside an executor that has a
unique executor ID, the task group is simply executed as a single isolated OS
process; this is the simple case of a single task.

When multiple tasks are specified for a single task group, all of the tasks will
be launched together on the same agent, and their lifecycles are coupled such
that if a single task fails, they are all killed. On Linux, the tasks will also
share network and mount namespaces by default so that they can communicate over
the network and access the same volumes (note that custom container networks may
be used as well). The resource constraints specified may be enforced for the
tasks collectively or individually depending on other settings; for more
information, see below, as well as the documentation on
[nested containers and task groups](nested-container-and-task-group.md).

## The Executor

The Mesos "executor" is responsible for managing the tasks. The executor must be
specified in the `LAUNCH_GROUP` operation, including an executor ID, the
framework ID, and some resources for the executor to perform its work. The
minimum resources required for an executor are shown in the example below.

## The Workload

You can specify your workload using a shell command, one or more artifacts to
be fetched before task launch, a container image, or some combination of these.
The example below shows a simple shell command and a URI pointing to a tarball
which presumably contains the script invoked in the command.

## Resource Requests and Limits

In each task, the resources required by that task can be specified. Common
resource types are `cpus`, `mem`, and `disk`. The resources listed in the
`resources` field are known as resource "requests" and represent the minimum
resource guarantee required by the task; these resources will always be
available to the task if they are needed. The quantities specified in the
`limits` field are the resource "limits", which represent the maximum amount of
`cpus` and/or `mem` that the task may use. Setting a CPU or memory limit higher
than the corresponding request allows the task to consume more than its
allocated amount of CPU or memory when there are unused resources available on
the agent. For important Linux-specific settings related to resource limits, see
the section below on Linux resource isolation.

In addition to finite numeric values, the resource limits may be set to
infinity, indicating that the task will be permitted to consume any available
CPU and/or memory on the agent. This is represented in the JSON example below
using the string "Infinity", though when submitting scheduler calls in protobuf
format the standard IEEE-defined floating point infinity value may be used.

When a task consumes extra available memory on an agent but then other task
processes on the machine which were guaranteed access to that memory suddenly
need it, it's possible that processes will have to be killed in order to
reclaim memory. When a task has a memory limit higher than its memory request,
the task process's OOM score adjustment is set so that it is OOM-killed
preferentially if it exceeds its memory request in such cases.

## Linux Resource Isolation

When workloads are executed on Linux agents, resource isolation is likely
provided by the Mesos agent's manipulation of cgroup subsystems. In the simple
case of an executor running a single task group with a single task (like the
example below), enforcement of resource requests and limits is straightforward,
since there is only one task process to isolate.

When multiple tasks or task groups run under a single executor, the enforcement
of resource constraints is more complex. Some control over this is allowed by
the `container.linux_info.share_cgroups` field in each task. When this boolean
field is `true` (this is the default), each task is constrained by the cgroups
of its executor. This means that if multiple tasks run underneath one executor,
their resource constraints will be enforced as a sum of all the task resource
constraints, applied collectively to those task processes. In this case, task
resource consumption is collectively managed via one set of cgroup subsystem
control files associated with the executor.

When the `share_cgroups` field is set to `false`, the resource consumption of
each task is managed via a unique set of cgroups associated with that task,
which means that each task process is subject to its own resource requests and
limits. Note that if you want to specify `limits` on a task, the task MUST set
`share_cgroups` to `false`. Also note that all tasks under a single executor
must share the same value of `share_cgroups`.

## Example: Launching a Task Group

The following could be submitted by a registered scheduler in the body of a POST
request to the Mesos master's `/api/v1/scheduler` endpoint:

```
{
  "framework_id": { "value" : "12220-3440-12532-2345" },
  "type": "ACCEPT",
  "accept": {
    "offer_ids": [ { "value" : "12220-3440-12532-O12" } ],
    "operations": [
      {
        "type": "LAUNCH_GROUP",
        "launch_group": {
          "executor": {
            "type": "DEFAULT",
            "executor_id": { "value": "28649-27G5-291H9-3816-04" },
            "framework_id": { "value" : "12220-3440-12532-2345" },
            "resources": [
              {
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 0.1 }
              }, {
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 32 }
              }, {
                "name": "disk",
                "type": "SCALAR",
                "scalar": { "value": 32 }
              }
            ]
          },
          "task_group": {
            "tasks": [
              {
                "name": "Name of the task",
                "task_id": {"value" : "task-000001"},
                "agent_id": {"value" : "83J792-S8FH-W397K-2861-S01"},
                "resources": [
                  {
                    "name": "cpus",
                    "type": "SCALAR",
                    "scalar": { "value": 1.0 }
                  }, {
                    "name": "mem",
                    "type": "SCALAR",
                    "scalar": { "value": 512 }
                  }, {
                    "name": "disk",
                    "type": "SCALAR",
                    "scalar": { "value": 1024 }
                  }
                ],
                "limits": {
                  "cpus": "Infinity",
                  "mem": 4096
                }
                "command": { "value": "./my-artifact/run.sh" },
                "container": {
                  "type": "MESOS",
                  "linux_info": { "share_cgroups": false }
                },
                "uris": [
                  { "value": "https://my-server.com/my-artifact.tar.gz" }
                ]
              }
            ]
          }
        }
      }
    ],
    "filters": { "refuse_seconds" : 5.0 }
  }
}
```

# Command Tasks

One or more simple tasks which specify a single container image and/or command
to execute can be launched using the `LAUNCH` operation. The same `TaskInfo`
message type is used in both the `LAUNCH_GROUP` and `LAUNCH` calls to describe
tasks, so the operations look similar and identical fields in the task generally
behave in the same way. Depending on the container type specified within the
task's `container` field, the task will be launched using either the Mesos
containerizer (Mesos in-tree container runtime) or the Docker containerizer
(wrapper around Docker runtime). Note that the
`container.linux_info.share_cgroups` field, if set, must be set to `true` for
command tasks.

The below example could be used as the payload of a POST request to the
scheduler API endpoint:

```
{
  "framework_id": { "value" : "12220-3440-12532-2345" },
  "type": "ACCEPT",
  "accept": {
    "offer_ids": [ { "value" : "12220-3440-12532-O12" } ],
    "operations": [
      {
        "type": "LAUNCH",
        "launch": {
          "task_infos": [
            {
              "name": "Name of the task",
              "task_id": {"value" : "task-000001"},
              "agent_id": {"value" : "83J792-S8FH-W397K-2861-S01"},
              "resources": [
                {
                  "name": "cpus",
                  "type": "SCALAR",
                  "scalar": { "value": 1.0 }
                }, {
                  "name": "mem",
                  "type": "SCALAR",
                  "scalar": { "value": 512 }
                }, {
                  "name": "disk",
                  "type": "SCALAR",
                  "scalar": { "value": 1024 }
                }
              ],
              "limits": {
                "cpus": "Infinity",
                "mem": 4096
              }
              "command": { "value": "./my-artifact/run.sh" },
              "container": {
                "type": "MESOS",
                "linux_info": { "share_cgroups": false }
              },
              "uris": [
                { "value": "https://my-server.com/my-artifact.tar.gz" }
              ]
            }
          ]
        }
      }
    ],
    "filters": { "refuse_seconds" : 5.0 }
  }
}
```
