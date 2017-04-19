---
title: Apache Mesos - Multiple Disks
layout: documentation
---

# Multiple Disks

Mesos provides a mechanism for operators to expose multiple disk resources. When
creating [persistent volumes](persistent-volume.md) frameworks can decide
whether to use specific disks by examining the `source` field on the disk
resources offered.

## Types of Disk Resources

`Disk` resources come in three forms:

* A `Root` disk is presented by not having the `source` set in `DiskInfo`.
* A `Path` disk is presented by having the `PATH` enum set for `source` in
  `DiskInfo`. It also has a `root` which the operator uses to specify the
  directory to be used to store data.
* A `Mount` disk is presented by having the `MOUNT` enum set for `source` in
  `DiskInfo`. It also has a `root` which the operator uses to specify the
  mount point used to store data.

Operators can use the JSON-formated `--resources` option on the agent to provide
these different kind of disk resources on agent start-up. Example resource
values in JSON format can be found below. By default (if `--resources` is not
specified), the Mesos agent will only make the root disk available to the
cluster.

**NOTE:** Once you specify any `Disk` resource manually (i.e., via the
`--resources` flag), Mesos will stop auto-detecting the `Root` disk resource.
Hence if you want to use the `Root` disk you will need to manually specify it
using the format described below.

### `Root` disk

A `Root` disk is the basic disk resource in Mesos. It usually maps to the
storage on the main operating system drive that the operator has presented to
the agent. Data is mapped into the `work_dir` of the agent.

An example resources value for a root disk is shown below. Note that the
operator could optionally specify a `role` for the disk, which would result in
[statically reserving](reservation.md) the disk for a single [role](roles.md).

        [
          {
            "name" : "disk",
            "type" : "SCALAR",
            "scalar" : { "value" : 2048 }
          }
        ]

### `Path` disks

A `Path` disk is an auxiliary disk resource provided by the operator. This
can be carved up into smaller chunks by creating persistent volumes that use
less than the total available space on the disk. Common uses for this kind of
disk are extra logging space, file archives or caches, or other non
performance-critical applications.  Operators can present extra disks on their
agents as `Path` disks simply by creating a directory and making that the `root`
of the `Path` in `DiskInfo`'s `source`.

`Path` disks are also useful for mocking up a multiple disk environment by
creating some directories on the operating system drive. This should only be
done in a testing or staging environment. Note that creating multiple `Path`
disks on the same filesystem requires statically partitioning the available disk
space. For example, suppose a 10GB storage device is mounted to `/foo` and the
Mesos agent is configured with two `Path` disks at `/foo/disk1` and
`/foo/disk2`. To avoid the risk of running out of space on the device, `disk1`
and `disk2` should be configured (when the Mesos agent is started) to use at
most 10GB of disk space in total.

An example resources value for a `Path` disk is shown below. Note that the
operator could optionally specify a `role` for the disk, which would result in
[statically reserving](reservation.md) the disk for a single [role](roles.md).

        [
          {
            "name" : "disk",
            "type" : "SCALAR",
            "scalar" : { "value" : 2048 },
            "disk" : {
              "source" : {
                "type" : "PATH",
                "path" : { "root" : "/mnt/data" }
              }
            }
          }
        ]

### `Mount` disks

A `Mount` disk is an auxiliary disk resource provided by the operator. This
__cannot__ be carved up into smaller chunks by frameworks. This lack of
flexibility allows operators to provide assurances to frameworks that they will
have exclusive access to the disk device. Common uses for this kind of disk
include database storage, write-ahead logs, or other performance-critical
applications.

On Linux, `Mount` disks must map to a `mount` point in the `/proc/mounts`
table. Operators should mount a physical disk with their preferred file system
and provide the mount point as the `root` of the `Mount` in `DiskInfo`'s
`source`.

Aside from the performance advantages of `Mount` disks, applications running on
them should be able to rely on disk errors when they attempt to exceed the
capacity of the volume. This holds true as long as the file system in use
correctly propagates these errors. Due to this expectation, the `disk/du`
isolation is disabled for `Mount` disks.

An example resources value for a `Mount` disk is shown below. Note that the
operator could optionally specify a `role` for the disk, which would result in
[statically reserving](reservation.md) the disk for a single [role](roles.md).

        [
          {
            "name" : "disk",
            "type" : "SCALAR",
            "scalar" : { "value" : 2048 },
            "disk" : {
              "source" : {
                "type" : "MOUNT",
                "mount" : { "root" : "/mnt/data" }
              }
            }
          }
        ]

#### `Block` disks

Mesos currently does not allow operators to expose raw block devices. It may do
so in the future, but there are security and flexibility concerns that need to
be addressed in a design document first.

### Implementation

A `Path` disk will have sub-directories created within the `root` which will be
used to differentiate the different volumes that are created on it. When a
persistent volume on a `Path` disk is destroyed, Mesos will remove all the files
and directories stored in the volume, as well as the sub-directory within `root`
that was created by Mesos for the volume.

A `Mount` disk will __not__ have sub-directories created, allowing applications
to use the full file system mounted on the device. This construct allows Mesos
tasks to access volumes that contain pre-existing directory structures. This can
be useful to simplify ingesting data such as a pre-existing Postgres database or
HDFS data directory. Note that when a persistent volume on a `Mount` disk is
destroyed, Mesos will remove all the files and directories stored in the volume,
but will __not__ remove the root directory (i.e., the mount point).

Operators should be aware of these distinctions when inspecting or cleaning up
remnant data.
