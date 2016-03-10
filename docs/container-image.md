---
title: Apache Mesos - Supporting Container Images in Mesos Containerizer
layout: documentation
---

# Supporting Container Images in [Mesos Containerizer](mesos-containerizer.md)


## Motivation

Mesos currently supports several [containerizers](containerizer.md),
notably the Mesos containerizer and the Docker containerizer. Mesos
containerizer uses native OS features directly to provide isolation
between containers, while Docker containerizer delegates container
management to the Docker engine.

Maintaining two containerizers is hard. For instance, when we add new
features to Mesos (e.g., persistent volumes, disk isolation), it
becomes a burden to update both containerizers. Even worse, sometimes
the isolation on some resources (e.g., network handles on an agent)
requires coordination between two containerizers, which is very hard
to implement in practice. In addition, we found that extending and
customizing isolation for containers launched by Docker engine is
difficult, mainly because we do not have a way to inject logics during
the life cycle of a container.

Therefore, we made an effort to unify containerizers in Mesos
([MESOS-2840](https://issues.apache.org/jira/browse/MESOS-2840),
a.k.a. the Unified Containerizer). We improved Mesos containerizer so
that it now supports launching containers that specify container
images (e.g., Docker/Appc images).


## Getting Started

To support container images, we introduced a new component in Mesos
containerizer, called image provisioner. Image provisioner is
responsible for pulling, caching and preparing container root
filesystems. It also extracts runtime configurations from container
images which will then be passed to the corresponding isolators for
proper isolation.

There are a few container image specifications, notably
[Docker](https://github.com/docker/docker/blob/master/image/spec/v1.md),
[Appc](https://github.com/appc/spec/blob/master/SPEC.md), and
[OCI](https://github.com/opencontainers/specs) (future). Currently, we
support Docker and Appc images. More details about what features are
supported or not can be found in the following sections.

**NOTE**: container image is only supported on Linux currently.

### Configure the agent

To enable container image support in Mesos containerizer, the operator
will need to specify the `--image_providers` agent flag which tells
Mesos containerizer what types of container images are allowed. For
example, setting `--image_providers=docker` allow containers to use
Docker images. The operators can also specify multiple container image
types. For instance, `--image_providers=docker,appc` allows both
Docker and Appc container images.

A few isolators need to be turned on in order to provide proper
isolation according to the runtime configurations specified in the
container image. The operator needs to add the following isolators to
the `--isolation` flag.

* `filesystem/linux`: This is needed because supporting container
images involves changing filesystem root, and only `filesystem/linux`
support that currently. Note that this isolator requires root
permission.

* `docker/runtime`: This is used to provide support for runtime
configurations specified in Docker images (e.g., Entrypoint/Cmd,
environment variables, etc.). See more details about this isolator in
[Mesos containerizer doc](mesos-containerizer.md). Note that if this
isolator is not specified and `--image_providers` contains `docker`,
the agent will refuse to start.

In summary, to enable container image support in Mesos containerizer,
please specify the following agent flags:

    $ sudo mesos-slave \
      --containerizers=mesos \
      --image_providers=appc,docker \
      --isolation=filesystem/linux,docker/runtime

### Framework API

We introduced a new protobuf message `Image` which allow frameworks to
specify container images for their containers. It has two types right
now: `APPC` and `DOCKER`, representing Appc and Docker images
respectively.

For Appc images, the `name` and `labels` are what described in the
[spec](https://github.com/appc/spec/blob/master/spec/aci.md#image-manifest-schema).

For Docker images, the `name` is the Docker image reference in the
following form (the same format expected by `docker pull`):
`[REGISTRY_HOST[:REGISTRY_PORT]/]REPOSITORY[:TAG|@DIGEST]`

    message Image {
      enum Type {
        APPC = 1;
        DOCKER = 2;
      }

      message Appc {
        required string name = 1;
        optional Labels labels = 3;
      }

      message Docker {
        required string name = 1;
      }

      required Type type = 1;

      // Only one of the following image messages should be set to match
      // the type.
      optional Appc appc = 2;
      optional Docker docker = 3;
    }

The framework needs to specify `MesosInfo` in `ContainerInfo` in order
to launch containers with container images. In other words, the
framework needs to set the type to `ContainerInfo.MESOS`, indicating
that it wants to use the Mesos containerizer. If `MesosInfo.image` is
not specified, the container will use the host filesystem. If
`MesosInfo.image` is specified, it will be used as the container
image when launching the container.

    message ContainerInfo {
      enum Type {
        DOCKER = 1;
        MESOS = 2;
      }

      message MesosInfo {
        optional Image image = 1;
      }

      required Type type = 1;
      optional MesosInfo mesos = 5;
    }

### Test it out!

First, start the Mesos master:

    $ sudo sbin/mesos-master --work_dir=/tmp/mesos/master

Then, start the Mesos agent:

    $ sudo GLOG_v=1 sbin/mesos-slave \
      --master=<MASTER_IP>:5050 \
      --isolation=docker/runtime,filesystem/linux \
      --work_dir=/tmp/mesos/slave \
      --image_providers=docker \
      --executor_environment_variables="{}"

Now, use Mesos CLI (i.e., mesos-execute) to launch a Docker container
(e.g., redis). Note that `--shell=false` tells Mesos to use the
default entrypoint and cmd specified in the Docker image.

    $ sudo bin/mesos-execute \
      --master=<MASTER_IP>:5050 \
      --name=test \
      --docker_image=library/redis \
      --shell=false

Verify if your container is running by launching a redis client:

    $ sudo docker run -ti --net=host redis redis-cli
    127.0.0.1:6379> ping
    PONG
    127.0.0.1:6379>


## Docker Support and Current Limitations

Image provisioner uses [Docker v2 registry
API](https://docs.docker.com/registry/spec/api/) to fetch Docker
images/layers. The fetching is based on `curl`, therefore SSL is
automatically handled. For private registries, the operator needs to
configure `curl` accordingly so that it knows where to find the
additional certificate files. Fetching requiring authentication is
currently not supported yet (coming soon).

Private registry is supported through the `--docker_registry` agent
flag. Specifying private registry for each container using
`Image.Docker.name` is not supported yet (coming soon).

If the `--docker_registry` agent flag points to a local directory
(e.g., `/tmp/mesos/images/docker`), the provisioner will pull Docker
images from local filesystem, assuming Docker archives (result of
`docker save`) are stored there based on the image name and tag.  For
example, the operator can put a `busybox:latest.tar` (the result of
`docker save -o busybox:latest.tar busybox`) under
`/tmp/mesos/images/docker` and launch the agent by specifying
`--docker_registry=/tmp/mesos/images/docker`. Then the framework can
launch a Docker container by specifying `busybox:latest` as the name
of the Docker image.

If the `--switch_user` flag is set on the agent and the framework
specifies a user (either `CommandInfo.user` or `FrameworkInfo.user`),
we expect that user exists in the container image and its uid and gids
matches that on the host. User namespace and capabilities are not
supported yet.

Only host network is supported. We will add bridge network support
soon using CNI support in Mesos
([MESOS-4641](https://issues.apache.org/jira/browse/MESOS-4641)).

### More agent flags

`--docker_registry`: The default URL for pulling Docker images. It
could either be a Docker registry server URL (i.e:
`https://registry.docker.io`), or a local path (i.e:
`/tmp/docker/images`) in which Docker image archives (result of
`docker save`) are stored. The default value is
`https://registry-1.docker.io`.

`--docker_store_dir`: Directory the Docker provisioner will store
images in. All the Docker images are cached under this directory. The
default value is `/tmp/mesos/store/docker`.


## Appc Support and Current Limitations

Currently, only the root filesystem specified in the Appc image is
supported. Other runtime configurations like environment variables,
exec, working directory are not supported yet (coming soon).

For image discovery, we current support a simple discovery mechanism.
We allow operators to specify a URI prefix which will be prepend to
the URI template `{name}-{version}-{os}-{arch}.{ext}`. For example, if
the URI prefix is `file:///tmp/appc/` and the Appc image name is
`example.com/reduce-worker` with `version:1.0.0`, we will fetch the
image at `file:///tmp/appc/example.com/reduce-worker-1.0.0.aci`.

### More agent flags

`appc_simple_discovery_uri_prefix`: URI prefix to be used for simple
discovery of appc images, e.g., `http://`, `https://`,
`hdfs://<hostname>:9000/user/abc/cde`. The default value is `http://`.

`appc_store_dir`: Directory the appc provisioner will store images in.
All the Appc images are cached under this directory. The default value
is `/tmp/mesos/store/appc`.


## Provisioner Backends

A provisioner backend takes a set of filesystem layers and stacks them
into a root filesystem. The following backends are supported
currently.

### Copy

The Copy backend simply copies all the layers into a target root
directory to create a root filesystem.

### Bind

This is a specialized backend that may be useful for deployments using
large (multi-GB) single-layer images *and* where more recent kernel
features such as overlayfs are not available. For small images (10's
to 100's of MB) the copy backend may be sufficient. Bind backend is
faster than Copy as it requires nearly zero IO.

The bind backend currently has these two limitations:

1. The bind backend supports only a single layer. Multi-layer images will
fail to provision and the container will fail to launch!

2. The filesystem is read-only because all containers using this image
share the source. Select writable areas can be achieved by mounting
read-write volumes to places like `/tmp`, `/var/tmp`, `/home`, etc.
using the `ContainerInfo`. These can be relative to the executor work
directory. Since the filesystem is read-only, `--sandbox_directory`
must already exist within the filesystem because the filesystem
isolator is unable to create it (e.g., either the image writer needs
to create the mount point in the image, or the operator needs to set
agent flag `--sandbox_directory` properly).

### Overlay

The reson overlay backend was introduced is because the copy backend
will waste IO and space while the bind backend can only deal with one
layer. The overlay backend allows containizer to utilize the
filesystem to merge multiple filesystems into one efficiently.

As the overlay backend request multiple lower layers support, the
version of linux kernel should be greater than 4.0, refer
[here](https://github.com/manjaro/manjaro-tools-iso-profiles/issues/40)
for more detail.

For more information of overlayfs, please refer to
[here](https://www.kernel.org/doc/Documentation/filesystems/overlayfs.txt).

The overlay backend currently has these two limitations:

1. The overlay backend only supports more than one single layer. One
single layer images will fail to provision and the container will fail
to launch! This limitation will be solved soon.

2. Similar to the bind backend, the filesystem will be read-only.
Please refer to the second limitation of the bind backend for more
details. We will resolve this limitation soon.


## Executor Dependencies in a Container Image

Mesos has this concept of executors. All tasks are launched by an
executor. For a general purpose executor (e.g., thermos) of a
framework (e.g., Aurora), requiring it and all its dependencies to be
present in all possible container images that a user might use is
not trivial.

In order to solve this issue, we propose a solution where we allow the
executor to run on the host filesystem (without a container image).
Instead, it can specify a `volume` whose source is an `Image`. Mesos
containerizer will provision the `image` specified in the `volume`,
and mount it under the sandbox directory. The executor can perform
`pivot_root` or `chroot` itself to enter the container root
filesystem. This is how we solve the command executor issue, avoiding
the requirement that it has to exist in all container images.


## References

For more information on the Mesos containerizer filesystem, namespace,
and isolator features, visit [Mesos
Containerizer](mesos-containerizer.md).  For more information on
launching Docker containers through the Docker containerizer, visit
[Docker Containerizer](docker-containerizer.md).
