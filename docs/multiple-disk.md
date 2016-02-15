---
layout: documentation
---

# Multiple Disks

Mesos provides a mechanism for operators to expose multiple disk resources. When
creating [persistent volumes](persistent-volume.md) frameworks can decide
whether to use specific disks by examining the `source` field on the disk
resources offered.

`Disk` resources come in three forms:

* A `Root` disk is presented by not having the `source` set in `DiskInfo`.
* A `Path` disk is presented by having the `PATH` enum set for `source` in
  `DiskInfo`. It also has a `root` which the operator uses to specify the
  directory to be used to store data.
* A `Mount` disk is presented by having the `MOUNT` enum set for `source` in
  `DiskInfo`. It also has a `root` which the operator uses to specify the
  mount point used to store data.

Operators can use the JSON-formated `--resources` option on the agent to provide
these different kind of disk resources on agent start-up.

#### `Root` disk

A `Root` disk is the basic disk resource in Mesos. It usually maps to the
storage on the main operating system drive that the operator has presented to
the agent. Data is mapped into the `work_dir` of the agent.

```
{
  "resources" : [
    {
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : <framework_role>
    }
  ]
}
```

#### `Path` disks

A `Path` disk is an auxiliary disk resource provided by the operator. This can
can be carved up into smaller chunks by accepting less than the total amount as
frameworks see fit. Common uses for this kind of disk are extra logging space,
file archives or caches, or other non performance-critical applications.
Operators can present extra disks on their agents as `Path` disks simply by
creating a directory and making that the `root` of the `Path` in `DiskInfo`'s
`source`.

`Path` disks are also useful for mocking up a multiple-disk environment by
creating some directories on the operating system drive. This should only be
done in a testing or staging environment.

```
{
  "resources" : [
    {
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : <framework_role>,
      "disk" : {
        "source" : {
          "type" : "PATH",
          "path" : { "root" : "/mnt/data" }
        }
      }
    }
  ]
}
```

#### `Mount` disks

A `Mount` disk is an auxiliary disk resource provided by the operator. This
__cannot__ be carved up into smaller chunks by frameworks. This lack of
flexibility allows operators to provide assurances to frameworks that they will
have exclusive access to the disk device. Common uses for this kind of disk
include database storage, write-ahead logs, or other performance-critical
applications.

Another requirement of `Mount` disks is that (on Linux) they map to a `mount`
point in the `/proc/mounts` table. Operators should mount a physical disk with
their preferred file system and provide the mount point as the `root` of the
`Mount` in `DiskInfo`'s `source`.

Aside from the performance advantages of `Mount` disks, applications running on
them should be able to rely on disk errors when they attempt to exceed the
capacity of the volume. This holds true as long as the file system in use
correctly propagates these errors. Due to this expectation, the `posix/disk`
quota enforcement is disabled for `Mount` disks.

```
{
  "resources" : [
    {
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : <framework_role>,
      "disk" : {
        "source" : {
          "type" : "MOUNT",
          "mount" : { "root" : "/mnt/data" }
        }
      }
    }
  ]
}
```

#### `Block` disks

Mesos currently does not allow operators to expose raw block devices. It may do
so in the future, but there are security and flexibility concerns that need to
be addressed in a design document first.

### Storage Management

Mesos currently does not clean up or destroy data when persistent volumes are
destroyed. It may do so in the future; however, the expectation is currently
upon the framework, executor, and application to delete their data before
destroying their persistent volumes. This is strongly encouraged for both
security and ensuring that future users of the underlying disk resource are not
penalized for prior consumption of the disk capacity.

### Implementation

A `Path` disk will have sub-directories created within the `root` which will be
used to differentiate the different volumes that are created on it.

A `Mount` disk will __not__ have sub-directories created, allowing applications
to use the full file system mounted on the device. This provides operators a
construct through which to enable data ingestion.

Operators should be aware of these distinctions when inspecting or cleaning up
remnant data.