---
layout: documentation
---

# Persistent Volumes

Mesos provides a mechanism to create a persistent volume from disk
resources. When launching a task, you can create a volume that exists outside
the task's sandbox and will persist on the node even after the task dies or
completes. When the task exits, its resources -- including the persistent volume
-- can be offered back to the framework, so that the framework can launch the
same task again, launch a recovery task, or launch a new task that consumes the
previous task's output as its input. Persistent volumes enable stateful services
such as HDFS and Cassandra to store their data within Mesos rather than having
to resort to workarounds (e.g., writing task state to a distributed filesystem
that is mounted at a well-known location outside the task's sandbox).

Persistent volumes can only be created from __reserved__ disk resources, whether
it be statically reserved or dynamically reserved. A dynamically reserved
persistent volume also cannot be unreserved without first explicitly destroying
the volume. These rules exist to limit accidental mistakes, such as a persistent
volume containing sensitive data being offered to other frameworks in the
cluster.

Please refer to the [Reservation](reservation.md) documentation for details
regarding reservation mechanisms available in Mesos.

Persistent volumes can be created by __operators__ and authorized
__frameworks__. We require a `principal` from the operator or framework in order
to authenticate/authorize the operations. Permissions are specified via the
existing ACL mechanism. To use authorization with reserve/unreserve operations,
the Mesos master must be configured with the desired ACLs. For more information,
see the [authorization documentation](authorization.md).

* `Offer::Operation::Create` and `Offer::Operation::Destroy` messages are
  available for __frameworks__ to send back via the `acceptOffers` API as a
  response to a resource offer.
* `/create-volumes` and `/destroy-volumes` HTTP endpoints allow
  __operators__ to manage persistent volumes through the master.

In the following sections, we will walk through examples of each of the
interfaces described above.

### Framework Scheduler API

#### `Offer::Operation::Create`

A framework can create volumes through the resource offer cycle.  Suppose we
receive a resource offer with 2048 MB of dynamically reserved disk.

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
an `Offer::Operation` message via the `acceptOffers` API.
`Offer::Operation::Create` has a `volumes` field which specifies the persistent
volume information. We need to specify the following:

1. The ID for the persistent volume; this must be unique per role on each slave.
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

If this succeeds, a subsequent resource offer will contain the following
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
      }
    }
  ]
}
```


#### `Offer::Operation::Destroy`

A framework can destroy persistent volumes through the resource offer cycle. In
[Offer::Operation::Create](#offeroperationcreate), we created a persistent
volume from 2048 MB of disk resources. Mesos will not garbage-collect this
volume until we explicitly destroy it. Suppose we would like to destroy the
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
specifies the persistent volumes to be destroyed.

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

If this request succeeds, the persistent volume will be destroyed but the disk
resources will still be reserved. As such, a subsequent resource offer will
contain the following reserved disk resources:

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

Those reserved resources can then be used as normal: e.g., they can be used to
create another persistent volume or can be unreserved.

Garbage collection for persistent volumes is planned but has not been
implemented yet -- [MESOS-2408](https://issues.apache.org/jira/browse/MESOS-2408).
In the mean time, even after you destroy a persistent volume, its content will
remain on disk.

### Operator HTTP Endpoints

As described above, persistent volumes can be created by a framework scheduler
as part of the resource offer cycle. Persistent volumes can also be created and
destroyed by sending HTTP requests to the `/create-volumes` and
`/destroy-volumes` endpoints, respectively. This capability is intended for use
by operators and administrative tools.

#### `/create-volumes`

To use this endpoint, the operator should first ensure that a reservation for
the necessary resources has been made on the appropriate slave (e.g., by using
the `/reserve` HTTP endpoint or by configuring a static reservation).

To create a 512MB persistent volume for the `ads` role on a dynamically reserved
disk resource, we can send a request like so:

```
curl -i \
     -u <operator_principal>:<password> \
     -d slaveId=<slave_id> \
     -d volumes='[
       {
         "name": "disk",
         "type": "SCALAR",
         "scalar": { "value": 512 },
         "role": "ads",
         "reservation": {
           "principal": <operator_principal>
         },
         "disk": {
           "persistence": {
             "id" : <persistence_id>
           },
           "volume": {
             "mode": "RW",
             "container_path": <path>
           }
         }
       }
     ]' \
     -X POST http://<ip>:<port>/master/create-volumes
```

The user receives one of the following HTTP responses:

* `200 OK`: Success (the persistent volumes have been created).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthorized request.
* `409 Conflict`: Insufficient resources to create the volumes.

Note that a single `/create-volumes` request can create multiple persistent
volumes, but all of the volumes must be on the same slave.

#### `/destroy-volumes`

To destroy the volume created above, we can send an HTTP POST like so:

```
curl -i \
     -u <operator_principal>:<password> \
     -d slaveId=<slave_id> \
     -d volumes='[
       {
         "name": "disk",
         "type": "SCALAR",
         "scalar": { "value": 512 },
         "role": "ads",
         "reservation": {
           "principal": <operator_principal>
         },
         "disk": {
           "persistence": {
             "id" : <persistence_id>
           },
           "volume": {
             "mode": "RW",
             "container_path": <path>
           }
         }
       }
     ]' \
     -X POST http://<ip>:<port>/master/destroy-volumes
```

The user receives one of the following HTTP responses:

* `200 OK`: Success (the volumes have been destroyed).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthorized request.
* `409 Conflict`: Insufficient resources to destroy the volumes.

Note that a single `/destroy-volumes` request can destroy multiple persistent
volumes, but all of the volumes must be on the same slave.
