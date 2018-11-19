---
layout: post
title: Run Mesos Locally with Mesos Mini Docker Container
published: true
post_author:
  display_name: Jie Yu
  twitter: jie_yu
tags: Docker
---

[Mesos Mini](https://hub.docker.com/r/mesos/mesos-mini/) is a Docker image maintained by the Apache Mesos community.
It allows you to test Mesos locally with a simple `docker run`.

# Why Mesos Mini

Being able to spin up a local Mesos cluster in Docker can greatly simplify the work in the following scenarios:

* *Demo*: Imagine doing a live demo with Mesos in a conference with unstable Wifi.
* *Framework development*: Write end-to-end integration tests for your framework with a local Mesos cluster in a Docker container.
  This can be easily automated in your test suite.
* *Test new Mesos features*: Test new features from Mesos that haven't been released yet.
  You might be able to do that by building Mesos from the source code, but most framework developers do not know how to do it, and it is slow.

The idea is similar to [minikube](https://github.com/kubernetes/minikube) or [minimesos](https://github.com/ContainerSolutions/minimesos).

However, [minimesos](https://github.com/ContainerSolutions/minimesos) is no longer maintained.
As a result, Apache Mesos community decides to maintain a solution in Mesos repository to simplify CI integrations.

# Get started

Make sure [Docker](https://docs.docker.com/install/) is installed.
We have tested on both Linux and MacOS.

To create a local Mesos cluster, simply do a `docker run`:

```bash
$ docker run --rm --privileged -p 5050:5050 -p 5051:5051 -p 8080:8080 mesos/mesos-mini
```

It will launch one Mesos master, one Mesos agent, and one example framework (Marathon) in the Docker container.

You should be able to access Mesos master UI at `http://localhost:5050`.
Similarly, you can access Mesos agent at `http://localhost:5051`.
Marathon UI can be accessed at `http://localhost:8080`.

You should be able to launch containers in the local Mesos cluster using Marathon like the following:

```bash
$ cat app.json
{
  "id": "test",
  "cmd": "sleep 1000",
  "cpus": 1,
  "mem": 128,
  "disk": 0,
  "instances": 1,
  "container": {
    "docker": {
      "image": "alpine"
    },
    "type": "DOCKER"
  },
  "networks": [
    {
      "mode": "host"
    }
  ]
}
$ curl -X POST -d @app.json -H "Content-type: application/json" http://localhost:8080/v2/apps
```

To stop the local Mesos cluster, please use `docker stop`.
All artifacts associated with the local Mesos cluster will be cleaned up when the Docker container stops.

The following Docker image tags are maintained:

* `master`: The latest master branch HEAD.
* `<RELEASE_BRANCH>`: The latest release branch HEAD (e.g., `1.7.x`).
* `master-<DATE>`: The snapshot builds for master branch (e.g., `master-2018-11-19`).
* `<RELEASE_BRANCH>-<DATE>`: The snapshot builds for release branch (e.g., `1.7.x-2018-11-19`).

Note that there is no support for release branches earlier than `1.7.x`.
All future release branches will be supported.

# How is it done?

## Manage multiple services

We use `systemd` to manage multiple daemons in the Mesos Mini Docker container.
As a result, you can use the following command to restart the Mesos master in the Mesos Mini Docker container:

```bash
$ docker exec <CONTAINER_ID> systemctl restart mesos-master
```

Similarly, you can use that to restart other services (e.g., Mesos agent or Marathon).
This is very useful for those end-to-end integration tests that want to simulate Mesos master failover.

## Docker Containerizer

One of the goals of Mesos Mini is to mimic production settings as much as possible.

To allow frameworks to launch Docker containers, we embed a Docker Daemon (i.e., Docker in Docker) in the Mesos Mini Docker container.
For instance, to view all Docker containers in the Mesos cluster, use the following command on your host:

```bash
$ docker exec <CONTAINER_ID> docker ps
```

The cgroup root for the embedded Docker Daemon has been configured so that the cgroups for the nested Docker containers are properly nested within the Mesos Mini Docker container.
This ensures that no cgroups traces will be left in the system when the Mesos Mini Docker container finishes.

## Mesos Containerizer (UCR)

For Mesos Containerizer (UCR), we turn on most of the [isolators](https://github.com/apache/mesos/docs/mesos-containerizer.md) that are typically turned on in production environments.
Similar to Docker daemon, we need to do a few tweaking on cgroups in Mesos Mini Docker container to make sure it does not leave any traces when Mesos Mini Docker container terminates.

For each cgroup subsystem, Docker does a bind mount from the current cgroup to the root of the cgroup subsystem.
For instance:

```bash
/sys/fs/cgroup/memory/docker/<cid> -> /sys/fs/cgroup/memory
```

This will confuse Mesos agent and UCR because it relies on proc file `/proc/<pid>/cgroup` to determine the cgroups of a given process, and this proc file is not affected by the bind mount of the cgroups.

To workaround that, we perform the following steps for each cgroup subsystems when bootstrapping the Mesos Mini Docker container to recreates the cgroups layout as if it were on the host.

```bash
$ mkdir -p /sys/fs/cgroup/memory/docker/<cid>
$ mount --bind /sys/fs/cgroup/memory /sys/fs/cgroup/memory/docker/<cid>
```

And then set Mesos agent `--cgroups_root` flag to `docker/<cid>`.

# Maintenance

The [build scripts](https://github.com/apache/mesos/tree/master/support/mesos-mini) for Mesos Mini is hosted in Mesos repository.
The [Mesos Docker Mini Jenkins CI](https://builds.apache.org/view/M-R/view/Mesos/job/Docker/job/Mini/) has been setup to automatically push daily snapshot builds to Docker hub for supported release branches as well as the master branch.

For any bug fix or new features, please follow the [Apache Mesos contribution guide](http://mesos.apache.org/community/#contribute-a-patch).

## Mesos CentOS Docker Image

In some scenarios, some users might prefer having a Docker image that only has Mesos installed.
To enable that, we also built a `mesos/mesos-centos` Docker image.
The tags are similar to those of Mesos Mini.
In fact, `mesos/mesos-mini` uses `mesos/mesos-centos` as its base image.

The [build scripts](https://github.com/apache/mesos/tree/master/support/packaging/centos/build-docker-centos.sh) for Mesos CentOS Docker image is also hosted in Mesos repository.
The [Mesos Docker CentOS Jenkins CI](https://builds.apache.org/view/M-R/view/Mesos/job/Docker/job/CentOS/) has been setup to automatically push daily snapshot builds to Docker hub for supported release branches as well as the master branch.

# Current limitations

* Only one Mesos agent can be launched in the Mesos Mini Docker container.
* Marathon uses in-memory storage instead of ZK.
* SSL is not enabled.
