---
title: Apache Mesos - Docker Containerizer
layout: documentation
---

# Docker Containerizer

Mesos 0.20.0 adds the support for launching tasks that contains Docker
images, with also a subset of Docker options supported while we plan
on adding more in the future.

Users can either launch a Docker image as a Task, or as an Executor.

The following sections will describe the API changes along with Docker
support, and also how to setup Docker.

## Setup

To run the agent to enable the Docker Containerizer, you must launch
the agent with "docker" as one of the containerizers option.

Example: `mesos-agent --containerizers=docker,mesos`

Each agent that has the Docker containerizer should have Docker CLI
client installed (version >= 1.8.0).

If you enable iptables on agent, make sure the iptables allow all
traffic from docker bridge interface through add below rule:

    iptables -A INPUT -s 172.17.0.0/16 -i docker0 -p tcp -j ACCEPT

## How do I use the Docker Containerizer?

TaskInfo before 0.20.0 used to only support either setting a
CommandInfo that launches a task running the bash command, or an
ExecutorInfo that launches a custom Executor that will launch the
task.

With 0.20.0 we added a ContainerInfo field to TaskInfo and
ExecutorInfo that allows a Containerizer such as Docker to be
configured to run the task or executor.

To run a Docker image as a task, in TaskInfo one must set both the
command and the container field as the Docker Containerizer will use
the accompanied command to launch the docker image.  The ContainerInfo
should have type Docker and a DockerInfo that has the desired docker
image.

To run a Docker image as an executor, in TaskInfo one must set the
ExecutorInfo that contains a ContainerInfo with type docker and the
CommandInfo that will be used to launch the executor.  Note that the
Docker image is expected to launch up as a Mesos executor that will
register with the agent once it launches.

## What does the Docker Containerizer do?

The Docker Containerizer is translating Task/Executor `Launch` and
`Destroy` calls to Docker CLI commands.

Currently the Docker Containerizer when launching as task will do the
following:

1. Fetch all the files specified in the CommandInfo into the sandbox.

2. Pull the docker image from the remote repository.

3. Run the docker image with the Docker executor, and map the sandbox
directory into the Docker container and set the directory mapping to
the MESOS_SANDBOX environment variable. The executor will also stream
the container logs into stdout/stderr files in the sandbox.

4. On container exit or containerizer destroy, stop and remove the
docker container.

The Docker Containerizer launches all containers with the `mesos-`
prefix plus the agent id (ie: `mesos-agent1-abcdefghji`), and also
assumes all containers with the `mesos-` prefix is managed by the
agent and is free to stop or kill the containers.

When launching the docker image as an Executor, the only difference is
that it skips launching a command executor but just reaps on the
docker container executor pid.

Note that we currently default to host networking when running a
docker image, to easier support running a docker image as an Executor.

The containerizer also supports optional force pulling of the image.
It is set disabled as default, so the docker image will only be
updated again if it's not available on the host. To enable force
pulling an image, `force_pull_image` has to be set as true.

## Private Docker repository

To run an image from a private repository, one can include the uri
pointing to a `.dockercfg` that contains login information.  The
`.dockercfg` file will be pulled into the sandbox the Docker
Containerizer set the HOME environment variable pointing to the
sandbox so docker cli will automatically pick up the config file.

Starting from 1.0, we provide an alternative way to specify docker
config file for pulling images from private registries. We allow
operators to specify a shared docker config file using an agent flag.
This docker config file will be used to pull images from private
registries for all containers. See [configuration
documentation](configuration/agent.md) for detail. Operators can either
specify the flag as an absolute path pointing to the docker config
file (need to manually configure `.docker/config.json` or `.dockercfg`
on each agent), or specify the flag as a JSON-formatted string.  For
example:

    --docker_config=file:///home/vagrant/.docker/config.json

or as a JSON object,

    --docker_config="{ \
      \"auths\": { \
        \"https://index.docker.io/v1/\": { \
          \"auth\": \"xXxXxXxXxXx=\", \
          \"email\": \"username@example.com\" \
        } \
      } \
    }"

## CommandInfo to run Docker images

A docker image currently supports having an entrypoint and/or a
default command.

To run a docker image with the default command (ie: `docker run
image`), the CommandInfo's value must not be set. If the value is set
then it will override the default command.

To run a docker image with an entrypoint defined, the CommandInfo's
shell option must be set to false.  If shell option is set to true the
Docker Containerizer will run the user's command wrapped with `/bin/sh
-c` which will also become parameters to the image entrypoint.

## Recover Docker containers on agent recovery

The Docker containerizer supports recovering Docker containers when
the agent restarts, which supports both when the agent is running in a
Docker container or not.

With the `--docker_mesos_image` flag enabled, the Docker containerizer
assumes the containerizer is running in a container itself and
modifies the mechanism it recovers and launches docker containers
accordingly.
