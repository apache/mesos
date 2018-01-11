---
title: Apache Mesos - Filesystem Isolators in Mesos Containerizer
layout: documentation
---

# Filesystem Isolators in Mesos Containerizer

The [Mesos Containerizer](../mesos-containerizer.md) has several 'filesystem'
isolators that are used to provide isolation for a container's filesystems.
Usually, each platform has a corresponding filesystem isolator associated with
it, because the level of isolation depends on the capabilities of that platform.

Currently, the Mesos Containerizer supports the
[`filesystem/posix`](#filesystemposix-isolator) and
[`filesystem/linux`](#filesystemlinux-isolator) isolators.
[`filesystem/shared`](filesystem-shared.md) isolator has a subset of the
features provided by the [`filesystem/linux`](#filesystemlinux-isolator)
isolator and is broken on hosts with systemd
([MESOS-6563](https://issues.apache.org/jira/browse/MESOS-6563)), thus is not
recommended and will be deprecated.

If you are using the Mesos Containerizer, at least one of the filesystem
isolators needs to be specified through the `--isolation` flag. If a user does
not specify any filesystem isolator, Mesos Containerizer will default to using
the [`filesystem/posix`](#filesystemposix-isolator) isolator.

Filesystem isolation is a pre-requisite for all the [container volume
isolators](../container-volume.md) because it provides some basic
functionality that the volume isolators depends on. For example, the
[`filesystem/linux`](#filesystemlinux-isolator) isolator will create a new mount
namespace for the container so that any volume mounts made by the volume
isolators will be hidden from the host mount namespace.

The filesystem isolator is also responsible for preparing [persistent volumes](../persistent-volume.md)
for containers.

## `filesystem/posix` isolator

The `filesystem/posix` isolator works on all POSIX systems. It isolates
container sandboxes and persistent volumes using UNIX file permissions.

All containers share the same host filesystem. As a result, if you want to
specify a [container image](../container-image.md) for the container, you cannot
use this isolator. Use the [`filesystem/linux`](#filesystemlinux-isolator)
isolator instead.

The `filesystem/posix` isolator handles [persistent volumes](../persistent-volume.md)
by creating symlinks in the container's sandbox that point to the actual
persistent volumes on the host filesystem.

## `filesystem/linux` isolator

The `filesystem/linux` isolator works only on Linux. It isolates the filesystems
of containers using the following primitives:

* Each container gets its own mount namespace. The default [mount propagation](https://www.kernel.org/doc/Documentation/filesystems/sharedsubtree.txt)
  in each container is set to 'slave'.
* Use UNIX file permissions to protect container sandboxes and persistent
  volumes.

Each container is allowed to define its own [image](../container-image.md). If a
container image is specified, by default, the container won't be able to see
files and directories on the host filesystem.

The `filesystem/linux` isolator handles [persistent volumes](../persistent-volume.md)
by bind mounting persistent volumes into the container's sandbox.
