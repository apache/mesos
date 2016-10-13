---
layout: documentation
---

# Shared Persistent Volumes

## Overview

Mesos already provides a mechanism to create persistent volumes. The persistent
volumes that are created on a specific agent is offered to the framework(s)
as resources. As a result, an executor/container which needs access to that
volume can use that resource. While the executor/container using the
persistent volume is running, it cannot be offered again to any framework
and hence would not be available to another executor/container.

Currently, access to regular persistent volumes is exclusive to a single
container/executor at a time. Shared persistent volumes allow multiple
executor/containers access to the same persistent volume simultaneously.
Simulatenous access to a single shared persistent volume is not isolated.

The epic for this feature is
[MESOS-3421](https://issues.apache.org/jira/browse/MESOS-3421).

Please refer to the following documents:

* [Persistent Volumes](persistent-volume.md): Documentation for details
  regarding persistent volumes available in Mesos.
* [Reservation](reservation.md): Documentation for details regarding
  reservation mechanisms available in Mesos.

Additional references:

* Talk at MesosCon Europe 2016 at Amsterdam on August 31, 2016 entitled
  ["Practical Persistent Volumes"]
  (http://schd.ws/hosted_files/mesosconeu2016/08/MesosConEurope2016PPVv1.0.pdf).

## Framework opt in for shared resources

A new `FrameworkInfo::Capability`, viz. `SHARED_RESOURCES` is added for a
framework to indicate that the framework is willing to accept shared
resources. If the framework registers itself with this capability, offers
shall contain the shared persistent volumes.

## Creation of shared Persistent Volume

The framework can create a shared persistent volume using the existing
persistent volume workflow. See usage examples for Scheduler API and
Operator HTTP Endpoints in [Persistent Volumes](persistent-volume.md).

Suppose a framework receives a resource offer with 2048 MB of dynamically
reserved disk.

```
{
  "id" : <offer_id>,
  "framework_id" : <framework_id>,
  "slave_id" : <slave_id>,
  "hostname" : <hostname>,
  "resources" : [
    {
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : <framework_role>,
      "reservation" : {
        "principal" : <framework_principal>
      }
    }
  ]
}
```

When creating a new persistent volume, the framework can marked it as shared
by setting the shared attribute.

```
{
  "type" : Offer::Operation::CREATE,
  "create": {
    "volumes" : [
      {
        "name" : "disk",
        "type" : "SCALAR",
        "scalar" : { "value" : 2048 },
        "role" : <framework_role>,
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

If this succeeds, a subsequent resource offer will contain the shared
persistent volume:

```
{
  "id" : <offer_id>,
  "framework_id" : <framework_id>,
  "slave_id" : <slave_id>,
  "hostname" : <hostname>,
  "resources" : [
    {
      "name" : "disk",
      "type" : "SCALAR",
      "scalar" : { "value" : 2048 },
      "role" : <framework_role>,
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
      "shared": {
      }
    }
  ]
}
```

The rest of the basic workflow is identical to the regular persistent
volumes. The same shared persistent volume is offered to frameworks
of the same role in different offer cycles.

## Unique Shared Persistent Volume Features

### Launching multiple tasks on the same shared persistent volume

Since a shared persisent volume is offered to frameworks even when that
volume is being used by an executor/container, a framework can launch
additional tasks using the same shared persistent volume. Moreover,
multiple tasks using the same shared persistent volume can be launched
in a single `ACCEPT` call (and does not necessarily need to spread across
multiple `ACCEPT` calls).

### Destroying shared persistent volumes

Since a shared persistent volume is offered to frameworks even if the
volume is currently in use, presence of a shared volume in the offer
does not automatically make that volume eligible to be destroyed. On
receipt of a `DESTROY`, the shared volume is destroyed only if there is
no running or pending task using that shared volume. If the volume is
already assigned to one or more executors/containers, the DESTROY of
the shared volume shall not be successful; and that shared volume will
be offered in the next cycle.
