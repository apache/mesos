---
title: Apache Mesos - Containerizers
layout: documentation
---

# Containerizers

## Motivation

Containerizers are used to run tasks in 'containers', which in turn are
used to:

* Isolate a task from other running tasks.
* 'Contain' tasks to run in limited resource runtime environment.
* Control a task's resource usage (e.g., CPU, memory) programatically.
* Run software in a pre-packaged file system image, allowing it to run in
  different environments.


## Types of containerizers

Mesos plays well with existing container technologies (e.g., docker) and also
provides its own container technology. It also supports composing different
container technologies (e.g., docker and mesos).

Mesos implements the following containerizers:

* [Composing](#Composing)
* [Docker](#Docker)
* [Mesos (default)](#Mesos)

User can specify the types of containerizers to use via the agent flag
`--containerizers`.


<a name="Composing"></a>
### Composing containerizer

This feature allows multiple container technologies to play together. It is
enabled when you configure the `--containerizers` agent flag with multiple comma
seperated containerizer names (e.g., `--containerizers=mesos,docker`). The order
of the comma separated list is important as the first containerizer that
supports the task's container configuration will be used to launch the task.

Use cases:

* For testing tasks with different types of resource isolations. Since 'mesos'
  containerizers have more isolation abilities, a framework can use composing
  containerizer to test a task using 'mesos' containerizer's controlled
  environment and at the same time test it to work with 'docker' containers by
  just changing the container parameters for the task.


<a name="Docker"></a>
### Docker containerizer

Docker containerizer allows tasks to be run inside docker container. This
containerizer is enabled when you configure the agent flag as
`--containerizers=docker`.

Use cases:

* If a task needs to be run with the tooling that comes with the docker package.
* If Mesos agent is running inside a docker container.

For more details, see
[Docker Containerizer](docker-containerizer.md).

<a name="Mesos"></a>
### Mesos containerizer

This containerizer allows tasks to be run with an array of pluggable isolators
provided by Mesos. This is the native Mesos containerizer solution and is
enabled when you configure the agent flag as `--containerizers=mesos`.

Use cases:

* Allow Mesos to control the task's runtime environment without depending on
  other container technologies (e.g., docker).
* Want fine grained operating system controls (e.g., cgroups/namespaces provided
  by Linux).
* Want Mesos's latest container technology features.
* Need additional resource controls like disk usage limits, which
  might not be provided by other container technologies.
* Want to add custom isolation for tasks.

For more details, see
[Mesos Containerizer](mesos-containerizer.md).


## References

* [Containerizer Internals](containerizer-internals.md) for
  implementation details of containerizers.
