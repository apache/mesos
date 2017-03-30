---
title: Apache Mesos - Shared Persistent Volumes
layout: documentation
---

# Shared Persistent Volumes

## Overview

By default, [persistent volumes](persistent-volume.md) provide
_exclusive_ access: once a task is launched using a persistent volume,
no other tasks can use that volume, and the volume will not appear in
any resource offers until the task that is using it has finished.

In some cases, it can be useful to share a volume between multiple tasks
running on the same agent. For example, this could be used to
efficiently share a large data set between multiple data analysis tasks.

## Creating Shared Volumes

Shared persistent volumes are created using the same workflow as normal
persistent volumes: by starting with a
[reserved resource](reservation.md) and applying a `CREATE` operation,
either via the framework scheduler API or the
[/create-volumes](endpoints/master/create-volumes.md) HTTP endpoint. To
create a shared volume, set the `shared` field during volume creation.

For example, suppose a framework subscribed to the `"engineering"` role
receives a resource offer containing 2048MB of dynamically reserved disk:

```
{
  "allocation_info": { "role": "engineering" },
  "id" : <offer_id>,
  "framework_id" : <framework_id>,
  "slave_id" : <slave_id>,
  "hostname" : <hostname>,
  "resources" : [
    {
      "allocation_info": { "role": "engineering" },
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : "engineering",
      "reservation" : {
        "principal" : <framework_principal>
      }
    }
  ]
}
```

The framework can create a shared persistent volume using this disk
resource via the following offer operation:

```
{
  "type" : Offer::Operation::CREATE,
  "create": {
    "volumes" : [
      {
        "allocation_info": { "role": "engineering" },
        "name" : "disk",
        "type" : "SCALAR",
        "scalar" : { "value" : 2048 },
        "role" : "engineering",
        "reservation" : {
          "principal" : <framework_principal>
        },
        "disk": {
          "persistence": {
            "id" : <persistent_volume_id>
          },
          "volume" : {
            "container_path" : <container_path>,
            "mode" : <mode>
          }
        },
        "shared" : {
        }
      }
    ]
  }
}
```

Note that the `shared` field has been set (to an empty JSON object),
which indicates that the `CREATE` operation will create a shared volume.

## Using Shared Volumes

To be eligible to receive resource offers that contain shared volumes, a
framework must enable the `SHARED_RESOURCES` capability in the
`FrameworkInfo` it provides when it registers with the master.
Frameworks that do _not_ enable this capability will not be offered
shared resources.

When a framework receives a resource offer, it can determine whether a
volume is shared by checking if the `shared` field has been set. Unlike
normal persistent volumes, a shared volume that is in use by a task will
continue to be offered to the frameworks subscribed to the volume's role;
this gives those frameworks the opportunity to launch additional tasks
that can access the volume. A framework can also launch multiple tasks
that access the volume using a single `ACCEPT` call.

Note that Mesos does not provide any isolation or concurrency control
between the tasks that are sharing a volume. Framework developers should
ensure that tasks that access the same volume do not conflict with one
another. This can be done via careful application-level concurrency
control, or by ensuring that the tasks access the volume in a read-only
manner. Mesos provides support for read-only access to volumes: as
described in the [persistent volume](persistent-volume.md)
documentation, tasks that are launched on a volume can specify a `mode`
of `"RO"` to use the volume in read-only mode.

### Destroying Shared Volumes

A persistent volume, whether shared or not, can only be destroyed if no
running or pending tasks have been launched using the volume. For
non-shared volumes, it is usually easy to determine when it is safe to
delete a volume. For shared volumes, the framework(s) that have launched
tasks using the volume typically need to coordinate to ensure (e.g., via
reference counting) that a volume is no longer being used before it is
destroyed.

### Resource Allocation

TODO: how do shared volumes influence resource allocation?

## References

* [MESOS-3421](https://issues.apache.org/jira/browse/MESOS-3421)
  contains additional information about the implementation of this
  feature.

* Talk at MesosCon Europe 2016 on August 31, 2016 entitled
  "[Practical Persistent Volumes](http://schd.ws/hosted_files/mesosconeu2016/08/MesosConEurope2016PPVv1.0.pdf)".
