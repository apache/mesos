---
title: Apache Mesos - Container Volumes
layout: documentation
---

# Container Volumes

For each volume a container specifies (i.e., `ContainerInfo.volumes`),
the following fields must be specified:

- `container_path`: Path in the container filesystem at which the
  volume will be mounted. If the path is a relative path, it is
  relative to the container's sandbox.

- `mode`: If the volume is read-only or read-write.

- `source`: Describe where the volume originates from. See more
  details in the following section.

## Volume Source Types

- [HOST\_PATH](#host_path-volume-source)
- [SANDBOX\_PATH](#sandbox_path-volume-source)
- [DOCKER\_VOLUME](#docker_volume-volume-source)
- [SECRET](#secret-volume-source)

### HOST\_PATH Volume Source

This volume source represents a path on the host filesystem. The path
can either point to a directory or a file (either a regular file or a
device file).

The following example shows a `HOST_PATH` volume that mounts
`/var/lib/mysql` on the host filesystem to the same location in the
container.

```json
{
  "container_path": "/var/lib/mysql",
  "mode": "RW",
  "source": {
    "type": "HOST_PATH",
    "host_path": {
      "path": "/var/lib/mysql"
    }
  }
}
```

The mode and ownership of the volume will be the same as that on the
host filesystem.

If you are using the [Mesos Containerizer](mesos-containerizer.md),
`HOST_PATH` volumes are handled by the `volume/host_path` isolator. To
enable this isolator, append `volume/host_path` to the `--isolation`
flag when starting the agent. This isolator depends on the
[`filesystem/linux`](isolators/filesystems.md#filesystemlinux-isolator)
isolator.

[Docker Containerizer](docker-containerizer.md) supports `HOST_PATH`
volume as well.

### SANDBOX\_PATH Volume Source

There are currently two types of `SANDBOX_PATH` volume sources:
[`SELF`](#self-type) and [`PARENT`](#parent-type).

If you are using [Mesos Containerizer](mesos-containerizer.md),
`SANDBOX_PATH` volumes are handled by the `volume/sandbox_path`
isolator.  To enable this isolator, append `volume/sandbox_path` to
the `--isolation` flag when starting the agent.

The [Docker Containerizer](docker-containerizer.md) only supports
`SELF` type `SANDBOX_PATH` volumes currently.

#### `SELF` Type

This represents a path in the container's own sandbox. The path can
point to either a directory or a file in the sandbox of the container.

The following example shows a `SANDBOX_PATH` volume from the
container's own sandbox that mount the subdirectory `tmp` in the
sandbox to `/tmp` in the container root filesystem. This will be
useful to cap the `/tmp` usage in the container (if disk isolator is
used and `--enforce_container_disk_quota` is turned on).

```json
{
  "container_path": "/tmp",
  "mode": "RW",
  "source": {
    "type": "SANDBOX_PATH",
    "sandbox_path": {
      "type": "SELF",
      "path": "tmp"
    }
  }
}
```

The ownership of the volume will be the same as that of the sandbox of
the container.

Note that `container_path` has to be an absolute path in this case. If
`container_path` is relative, that means it's a volume from a
subdirectory in the container sandbox to another subdirectory in the
container sandbox. In that case, the user can just create a symlink,
instead of using a volume.

#### PARENT Type

This represents a path in the sandbox of the parent container. The
path can point to either a directory or a file in the sandbox of the
parent container. See the [nested container
doc](nested-container-and-task-group.md) for more details about what a
parent container is.

The following example shows a `SANDBOX_PATH` volume from the sandbox
of the parent container that mounts the subdirectory `shared_volume` in
the sandbox of the parent container to subdirectory `volume` in the
sandbox of the container.

```json
{
  "container_path": "volume",
  "mode": "RW",
  "source": {
    "type": "SANDBOX_PATH",
    "sandbox_path": {
      "type": "PARENT",
      "path": "shared_volume"
    }
  }
}
```

The ownership of the volume will be the same as that of the sandbox of
the parent container.

### DOCKER\_VOLUME Volume Source

See more details in this [doc](isolators/docker-volume.md).

### SECRET Volume Source

See more details in this [doc](secrets.md#file-based-secrets).
