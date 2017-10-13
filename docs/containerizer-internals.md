---
title: Apache Mesos - Containerizer Internals
layout: documentation
---


# Containerizer

Containerizers are Mesos components responsible for launching
containers. They own the containers launched for the tasks/executors,
and are responsible for their isolation, resource management, and
events (e.g., statistics).

# Containerizer internals

### Containerizer creation and launch

* Agent creates a containerizer based on the flags (using agent flag
  `--containerizers`). If multiple containerizers (e.g., docker,
  mesos) are specified using the `--containerizers` flag, then the
  composing containerizer will be used to create a containerizer.
* If an executor is not specified in `TaskInfo`, Mesos agent will use
  the default executor for the task (depending on the Containerizer
  the agent is using, it could be `mesos-executor` or
  `mesos-docker-executor`). TODO: Update this after MESOS-1718 is
  completed. After this change, master will be responsible for
  generating executor information.

### Types of containerizers

Mesos currently supports the following containerizers:

* Composing
* [Docker](docker-containerizer.md)
* [Mesos](mesos-containerizer.md)

#### Composing Containerizer

Composing containerizer will compose the specified containerizers
(using agent flag `--containerizers`) and act like a single
containerizer. This is an implementation of the `composite` design
pattern.

#### Docker Containerizer

Docker containerizer manages containers using the docker engine provided
in the docker package.

##### Container launch

* Docker containerizer will attempt to launch the task in docker only
  if `ContainerInfo::type` is set to DOCKER.
* Docker containerizer will first pull the image.
* Calls pre-launch hook.
* The executor will be launched in one of the two ways:

A) Mesos agent runs in a docker container

* This is indicated by the presence of agent flag
  `--docker_mesos_image`. In this case, the value of flag
  `--docker_mesos_image` is assumed to be the docker image used to
  launch the Mesos agent.
* If the task includes an executor (custom executor), then that executor is
  launched in a docker container.
* If the task does not include an executor i.e. it defines a command, the
  default executor `mesos-docker-executor` is launched in a docker container to
  execute the command via Docker CLI.

B) Mesos agent does not run in a docker container

* If the task includes an executor (custom executor), then that executor is
  launched in a docker container.
* If task does not include an executor i.e. it defines a command, a subprocess
  is forked to execute the default executor `mesos-docker-executor`.
  `mesos-docker-executor` then spawns a shell to execute the command via Docker
  CLI.

#### Mesos Containerizer

Mesos containerizer is the native Mesos containerizer. Mesos
Containerizer will handle any executor/task that does not specify
`ContainerInfo::DockerInfo`.

##### Container launch

* Calls prepare on each isolator.
* Forks the executor using Launcher (see [Launcher](#Launcher)). The
  forked child is blocked from executing until it is been isolated.
* Isolate the executor. Call isolate with the pid for each isolator
  (see [Isolators](#Isolators)).
* Fetch the executor.
* Exec the executor. The forked child is signalled to continue. It
  will first execute any preparation commands from isolators and then
  exec the executor.

<a name="Launcher"></a>
##### Launcher

Launcher is responsible for forking/destroying containers.

* Forks a new process in the containerized context. The child will
  exec the binary at the given path with the given argv, flags, and
  environment.
* The I/O of the child will be redirected according to the specified
  I/O descriptors.

###### Linux launcher

* Creates a "freezer" cgroup for the container.
* Creates posix "pipe" to enable communication between host (parent
  process) and container process.
* Spawn child process (container process) using `clone` system call.
* Moves the new container process to the freezer hierarchy.
* Signals the child process to continue (exec'ing) by writing a
  character to the write end of the pipe in the parent process.

Starting from Mesos 1.1.0, [nested container](nested-container-and-task-group.md)
is supported. The Linux Launcher is responsible to fork the subprocess
for the nested container with appropriate Linux namespaces being
cloned. The following is the table for Linux namespaces that
are supported for top level and nested containers.

###### Linux Namespaces

<table class="table table-striped">
  <tr>
    <th>Linux Namespaces</th>
    <th>Top Level Container</th>
    <th>Nested Container</th>
  </tr>
  <tr>
    <td>Mount</td>
    <td>Not shared</td>
    <td>Not shared</td>
  </tr>
  <tr>
    <td>PID</td>
    <td>Configurable</td>
    <td>Configurable</td>
  </tr>
  <tr>
    <td>Network & UTS</td>
    <td>Configurable</td>
    <td>Shared w/ parent</td>
  </tr>
  <tr>
    <td>IPC</td>
    <td>Not shared -> configurable (TBD)</td>
    <td>Not shared -> configurable (TBD)</td>
  </tr>
  <tr>
    <td>Cgroup</td>
    <td>Shared w/ agent -> Not shared (TBD)</td>
    <td>Shared w/ parent -> Not shared (TBD)</td>
  </tr>
  <tr>
    <td>User (not supported)</td>
    <td>Shared w/ agent</td>
    <td>Shared w/ parent</td>
  </tr>
</table>

*Note: For the top level container, `shared` means that the container
shares the namespace from the agent. For the nested container, `shared`
means that the nested container shares the namespace from its parent
container.

###### Posix launcher (TBD)

<a name="Isolators"></a>
##### [Isolators](mesos-containerizer.md#isolators)

[Isolators](mesos-containerizer.md#isolators) are responsible for creating
an environment for the containers where resources like cpu, network,
storage and memory can be isolated from other containers.

### Containerizer states

#### Docker

* FETCHING
* PULLING
* RUNNING
* DESTROYING

#### Mesos

* PREPARING
* ISOLATING
* FETCHING
* RUNNING
* DESTROYING
