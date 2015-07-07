---
layout: documentation
---

# Persistent Volume

Mesos provides a mechanism to create a persistent volume from disk resources.
This enables stateful services such as HDFS and Cassandra to store their data
within Mesos rather than having to resort to network-mounted EBS volumes that
needs to be placed in a well-known location.

Persistent volumes can only be created from __reserved__ disk resources, whether
it be statically reserved or dynamically reserved. A dynamically reserved
persistent volume also cannot be unreserved without having explicitly destroyed
the volume. These rules exist to limit the accidental mistakes such as:
a persistent volume containing sensitive data being offered to other frameworks
in the cluster.

Please refer to the
[Reservation](reservation.md) documentation for details regarding reservation
mechanisms available in Mesos.

Persistent volumes can be created by __operators__ and authorized
__frameworks__. We require a `principal` from the operator or framework in order
to authenticate/authorize the operations. [Authorization](authorization.md) is
specified via the existing ACL mechanism. (___Coming Soon___)

* `Offer::Operation::Create` and `Offer::Operation::Destroy` messages are
  available for __frameworks__ to send back via the `acceptOffers` API as a
  response to a resource offer.
* `/create` and `/destroy` HTTP endpoints are available for __operators__
  to manage persistent volumes through the master. (___Coming Soon___).

In the following sections, we will walk through examples of each of the
interfaces described above.


## `Offer::Operation::Create`

A framework is able to create volumes through the resource offer cycle.
Suppose we receive a resource offer with 2048 MB of dynamically reserved disk.

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

We can create a persistent volume from the 2048 MB of disk resources by sending
the following `Offer::Operation` message via the `acceptOffers` API.
`Offer::Operation::Create` has a `volumes` field which we specify with the
persistent volume information. We need to specify the following:

1. ID of the persistent volume which needs to be unique per role on each slave.
1. The non-nested relative path within the container to mount the volume.
1. The permissions for the volume. Currently, `"RW"` is the only possible value.

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
        }
      }
    ]
  }
}
```

The subsequent resource offer will __contain__ the following persistent volume:

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
      }
    }
  ]
}
```


## `Offer::Operation::Destroy`

A framework is able to destroy persistent volumes through the resource offer
cycle. In [Offer::Operation::Create](#offeroperationcreate), we created a
persistent volume from 2048 MB of disk resources. Mesos will not garbage-collect
this volume until we explicitly destroy it. Suppose we would like to destroy the
volume we created. First, we receive a resource offer (copy/pasted from above):

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
      }
    }
  ]
}
```

We destroy the persistent volume by sending the `Offer::Operation` message via
the `acceptOffers` API. `Offer::Operation::Destroy` has a `volumes` field which
we specify the persistent volumes to be destroyed.

```
{
  "type" : Offer::Operation::DESTROY,
  "destroy" : {
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
        }
      }
    ]
  }
}
```

The persistent volume will be destroyed, but the disk resources will still be
reserved. As such, the subsequent resource offer will __contain__ the following
reserved disk resources:

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

Note that in 0.23, even after you destroy the persistent volume, its content
will still be on the disk. The garbage collection for persistent volumes is
coming soon: [MESOS-2048](https://issues.apache.org/jira/browse/MESOS-2408).


### `/create` (_Coming Soon_)
### `/destroy` (_Coming Soon_)
