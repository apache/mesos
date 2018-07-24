---
title: Apache Mesos - Docker Volume Support in Mesos Containerizer
layout: documentation
---

# Docker Volume Support in Mesos Containerizer

Mesos 1.0 adds Docker volume support to the
[MesosContainerizer](../mesos-containerizer.md) (a.k.a., the universal
containerizer) by introducing the new `docker/volume` isolator.

This document describes the motivation, overall architecture, configuration
steps for enabling Docker volume isolator, and required framework changes.

## Table of Contents
- [Motivation](#motivation)
- [How does it work?](#how-does-it-work)
- [Configuration](#configuration)
  - [Pre-conditions](#pre-conditions)
  - [Configuring Docker Volume Isolator](#configure-Docker-volume-isolator)
  - [Enabling frameworks to use Docker volumes](#enable-frameworks)
    - [Volume Protobuf](#volume-protobuf)
    - [Examples](#examples)
- [Limitations](#limitations)
- [Test it out!](#test-it-out)

## <a name="motivation"></a>Motivation

The integration of external storage in Mesos is an attractive feature.  The
Mesos [persistent volume](../persistent-volume.md) primitives allow stateful
services to persist data on an agent's local storage. However, the amount of
storage capacity that can be directly attached to a single agent is
limited---certain applications (e.g., databases) would like to access more data
than can easily be attached to a single node. Using external storage can
also simplify data migration between agents/containers, and can make backups and
disaster recovery easier.

The [Docker Volume Driver
API](https://github.com/Docker/Docker/blob/master/docs/extend/plugins_volume.md)
defines an interface between the container runtime and external storage systems.
It has been widely adopted. There are Docker volume plugins for a variety of
storage drivers, such as [Convoy](https://github.com/rancher/convoy),
[Flocker](https://docs.clusterhq.com/en/latest/Docker-integration/),
[GlusterFS](https://github.com/calavera/Docker-volume-glusterfs), and
[REX-Ray](https://github.com/emccode/rexray). Each plugin typically supports a
variety of external storage systems, such as Amazon EBS, OpenStack Cinder, etc.

Therefore, introducing support for external storage in Mesos through the
`docker/volume` isolator provides Mesos with tremendous flexibility to
orchestrate containers on a wide variety of external storage technologies.

## <a name="how-does-it-work"></a>How does it work?

![Docker Volume Isolator Architecture](images/docker-volume-isolator.png)

The `docker/volume` isolator interacts with Docker volume plugins using
[dvdcli](https://github.com/emccode/dvdcli), an open-source command line tool
from EMC.

When a new task with Docker volumes is launched, the `docker/volume` isolator
will invoke [dvdcli](https://github.com/emccode/dvdcli) to mount the
corresponding Docker volume onto the host and then onto the container.

When the task finishes or is killed, the `docker/volume` isolator will invoke
[dvdcli](https://github.com/emccode/dvdcli) to unmount the corresponding Docker
volume.

The detailed workflow for the `docker/volume` isolator is as follows:

1. A framework specifies external volumes in `ContainerInfo` when launching a
   task.

2. The master sends the launch task message to the agent.

3. The agent receives the message and asks all isolators (including the
   `docker/volume` isolator) to prepare for the container with the
   `ContainerInfo`.

4. The isolator invokes [dvdcli](https://github.com/emccode/dvdcli) to mount the
   corresponding external volume to a mount point on the host.

5. The agent launches the container and bind-mounts the volume into the
   container.

6. The bind-mounted volume inside the container will be unmounted from the
   container automatically when the container finishes, as the container is in
   its own mount namespace.

7. The agent invokes isolator cleanup which invokes
   [dvdcli](https://github.com/emccode/dvdcli) to unmount all mount points for
   the container.

## <a name="configuration"></a>Configuration

To use the `docker/volume` isolator, there are certain actions required by
operators and framework developers. In this section we list the steps required
by the operator to configure `docker/volume` isolator and the steps required by
framework developers to specify the Docker volumes.

### <a name="pre-conditions"></a>Pre-conditions

- Install `dvdcli` version
  [0.1.0](https://github.com/emccode/dvdcli/releases/tag/v0.1.0) on each agent.

- Install the [Docker volume
  plugin](https://github.com/Docker/Docker/blob/master/docs/extend/plugins.md#volume-plugins)
on each agent.

- Explicitly create the Docker volumes that are going to be accessed by Mesos
  tasks. If this is not done, volumes will be implicitly created by
  [dvdcli](https://github.com/emccode/dvdcli) but the volumes may not fit into
  framework resource requirement well.

### <a name="configure-Docker-volume-isolator"></a>Configuring Docker Volume Isolator

In order to configure the `docker/volume` isolator, the operator needs to
configure two flags at agent startup as follows:

```{.console}
  sudo mesos-agent \
    --master=<master IP> \
    --ip=<agent IP> \
    --work_dir=/var/lib/mesos \
    --isolation=filesystem/linux,docker/volume \
    --docker_volume_checkpoint_dir=<mount info checkpoint path>
```

The `docker/volume` isolator must be specified in the `--isolation` flag at
agent startup; the `docker/volume` isolator has a dependency on the
`filesystem/linux` isolator.

The `--docker_volume_checkpoint_dir` is an optional flag with a default value of
`/var/run/mesos/isolators/docker/volume`. The `docker/volume` isolator will
checkpoint all Docker volume mount point information under
`--docker_volume_checkpoint_dir` for recovery. The checkpoint information under
the default `--docker_volume_checkpoint_dir` will be cleaned up after agent
restart. Therefore, it is recommended to set `--docker_volume_checkpoint_dir` to
a directory which will survive agent restart.

### <a name="enable-frameworks"></a>Enabling frameworks to use Docker volumes

#### <a name="volume-protobuf"></a>Volume Protobuf

The `Volume` protobuf message has been updated to support Docker volumes.

```{.proto}
message Volume {
  ...

  required string container_path = 1;

  message Source {
    enum Type {
      UNKNOWN = 0;
      DOCKER_VOLUME = 1;
    }

    message DockerVolume {
      optional string driver = 1;
      required string name = 2;
      optional Parameters driver_options = 3;
    }

    optional Type type = 1;
    optional DockerVolume docker_volume = 2;
  }

  optional Source source = 5;
}
```

When requesting a Docker volume for a container, the framework developer needs to
set `Volume` for the container, which includes `mode`, `container_path` and
`source`.

The `source` field specifies where the volume comes from. Framework developers need to
specify the `type`, Docker volume `driver`, `name` and `options`. At present,
only the `DOCKER_VOLUME` type is supported; we plan to add support for more
types of volumes in the future.

How to specify `container_path`:

1. If you are launching a Mesos container `without rootfs`. If `container_path`
   is an absolute path, you need to make sure the absolute path exists on your
   host root file system as the container shares the host root file system;
   otherwise, the task will fail.

2. For other cases like launching a Mesos container `without rootfs` and
   `container_path` is a relative path, or launching a task `with rootfs` and
   `container_path` is an absolute path, or launching a task `with rootfs` and
   `container_path` as a relative path, the isolator will help create the
   `container_path` as the mount point.

The following table summarizes the above rules for `container_path`:

<table class="table table-striped">
  <tr>
    <th></th>
    <th>Container with rootfs</th>
    <th>Container without rootfs</th>
  </tr>
  <tr>
    <td>Absolute container_path</td>
    <td>No need to exist</td>
    <td>Must exist</td>
  </tr>
  <tr>
    <td>Relative container_path</td>
    <td>No need to exist</td>
    <td>No need to exist</td>
  </tr>
</table>

#### <a name="examples"></a>Examples

1. Launch a task with one Docker volume using the default command executor.

   ```{.json}
   TaskInfo {
     ...
     "command" : ...,
     "container" : {
       "volumes" : [
         {
           "container_path" : "/mnt/volume",
           "mode" : "RW",
           "source" : {
             "type" : "DOCKER_VOLUME",
             "docker_volume" : {
               "driver" : "rexray",
               "name" : "myvolume"
             }
           }
         }
       ]
     }
   }
   ```

2. Launch a task with two Docker volumes using the default command executor.


   ```{.json}
   TaskInfo {
     ...
     "command" : ...,
     "container" : {
       "volumes" : [
         {
           "container_path" : "volume1",
           "mode" : "RW",
           "source" : {
             "type" : "DOCKER_VOLUME",
             "docker_volume" : {
               "driver" : "rexray",
               "name" : "volume1"
             }
           }
         },
         {
           "container_path" : "volume2",
           "mode" : "RW",
           "source" : {
             "type" : "DOCKER_VOLUME",
             "docker_volume" : {
               "driver" : "rexray",
               "name" : "volume2",
               "driver_options" : {
                 "parameter" : [{
                   "key" : <key>,
                   "value" : <value>
                 }, {
                   "key" : <key>,
                   "value" : <value>
                 }]
               }
             }
           }
         }
       ]
     }
   }
   ```

**NOTE**: The task launch will be failed if one container uses multiple Docker
volumes with the same `driver` and `name`.

## <a name="limitations"></a>Limitations

Using the same Docker volume in both the [Docker
Containerizer](../docker-containerizer.md) and the [Mesos
Containerizer](../mesos-containerizer.md) simultaneously is **strongly
discouraged**, because the MesosContainerizer has its own reference
counting to decide when to unmount a Docker volume. Otherwise, it
would be problematic if a Docker volume is unmounted by
MesosContainerizer but the DockerContainerizer is still using it.

## <a name="test-it-out"></a>Test it out!

This section presents examples for launching containers with Docker volumes.
The following example is using [convoy](https://github.com/rancher/convoy/)
as the Docker volume driver.

Start the Mesos master.

```{.console}
  $ sudo mesos-master --work_dir=/tmp/mesos/master
```

Start the Mesos agent.

```{.console}
  $ sudo mesos-agent \
    --master=<MASTER_IP>:5050 \
    --isolation=docker/volume,docker/runtime,filesystem/linux \
    --work_dir=/tmp/mesos/agent \
    --image_providers=docker \
    --executor_environment_variables="{}"
```

Create a volume named as `myvolume` with
[convoy](https://github.com/rancher/convoy/).

```{.console}
  $ convoy create myvolume
```

Prepare a volume json file named as `myvolume.json` with following content.

```
  [{
    "container_path":"\/tmp\/myvolume",
    "mode":"RW",
    "source":
    {
      "docker_volume":
        {
          "driver":"convoy",
          "name":"myvolume"
        },
        "type":"DOCKER_VOLUME"
    }
  }]
```

Now, use Mesos CLI (i.e., mesos-execute) to launch a Docker container with
`--volumes=<path>/myvolume.json` option.

```{.console}
  $ sudo mesos-execute \
    --master=<MASTER_IP>:5050 \
    --name=test \
    --docker_image=ubuntu:14.04 \
    --command="touch /tmp/myvolume/myfile" \
    --volumes=<path>/myvolume.json
```

Create another task to verify the file `myfile` was created successfully.

```{.console}
  $ sudo mesos-execute \
    --master=<MASTER_IP>:5050 \
    --name=test \
    --docker_image=ubuntu:14.04 \
    --command="ls /tmp/myvolume" \
    --volumes=<path>/myvolume.json
```

Check the [sandbox](../sandbox.md#where-is-it)
for the second task to check the file `myfile` was created successfully.

```{.console}
  $ cat stdout
    Received SUBSCRIBED event
    Subscribed executor on mesos002
    Received LAUNCH event
    Starting task test
    Forked command at 27288
    sh -c 'ls /tmp/myvolume/'
    lost+found
    myfile
    Command exited with status 0 (pid: 27288)
```
