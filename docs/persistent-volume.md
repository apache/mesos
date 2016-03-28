---
title: Apache Mesos - Persistent Volumes
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

## Usage

Persistent volumes can only be created from __reserved__ disk resources, whether
it be statically reserved or dynamically reserved. A dynamically reserved
persistent volume also cannot be unreserved without first explicitly destroying
the volume. These rules exist to limit accidental mistakes, such as a persistent
volume containing sensitive data being offered to other frameworks in the
cluster. Similarly, a persistent volume cannot be destroyed if there is an
active task that is still using the volume.

Please refer to the [Reservation](reservation.md) documentation for details
regarding reservation mechanisms available in Mesos.

Persistent volumes can also be created on isolated and auxiliary disks by
reserving [multiple disk resources](multiple-disk.md).

Persistent volumes can be created by __operators__ and authorized
__frameworks__. By default, frameworks and operators can create volumes for _any_
role and destroy _any_ persistent volume. [Authorization](authorization.md)
allows this behavior to be limited so that volumes can only be created for
particular roles and only particular volumes can be destroyed. For these
operations to be authorized, the framework or operator should provide a
`principal` to identify itself. To use authorization with reserve, unreserve,
create, and destroy operations, the Mesos master must be configured with the
appropriate ACLs. For more information, see the
[authorization documentation](authorization.md).

* `Offer::Operation::Create` and `Offer::Operation::Destroy` messages are
  available for __frameworks__ to send back via the `acceptOffers` API as a
  response to a resource offer.
* `/create-volumes` and `/destroy-volumes` HTTP endpoints allow
  __operators__ to manage persistent volumes through the master.

When a persistent volume is destroyed, all the data on that volume is removed
from the slave's filesystem. Note that for persistent volumes created on `Mount`
disks, the root directory is not removed, because it is typically the mount
point used for a separate storage device.

In the following sections, we will walk through examples of each of the
interfaces described above.

## Framework Scheduler API

### `Offer::Operation::Create`

A framework can create volumes through the resource offer cycle.  Suppose we
receive a resource offer with 2048 MB of dynamically reserved disk.

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

We can create a persistent volume from the 2048 MB of disk resources by sending
an `Offer::Operation` message via the `acceptOffers` API.
`Offer::Operation::Create` has a `volumes` field which specifies the persistent
volume information. We need to specify the following:

1. The ID for the persistent volume; this must be unique per role on each slave.
1. The non-nested relative path within the container to mount the volume.
1. The permissions for the volume. Currently, `"RW"` is the only possible value.

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

If this succeeds, a subsequent resource offer will contain the following
persistent volume:

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

### `Offer::Operation::Destroy`

A framework can destroy persistent volumes through the resource offer cycle. In
[Offer::Operation::Create](#offeroperationcreate), we created a persistent
volume from 2048 MB of disk resources. The volume will continue to exist until
it is explicitly destroyed. Suppose we would like to destroy the volume we
created. First, we receive a resource offer (copy/pasted from above):

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

We can destroy the persistent volume by sending a `Offer::Operation` message via
the `acceptOffers` API. `Offer::Operation::Destroy` has a `volumes` field which
specifies the persistent volumes to be destroyed.

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

If this request succeeds, the persistent volume will be destroyed, and all the
files and directories associated with the volume will be deleted. However, the
disk resources will still be reserved. As such, a subsequent resource offer will
contain the following reserved disk resources:

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

Those reserved resources can then be used as normal: e.g., they can be used to
create another persistent volume or can be unreserved.

## Operator HTTP Endpoints

As described above, persistent volumes can be created by a framework scheduler
as part of the resource offer cycle. Persistent volumes can also be created and
destroyed by sending HTTP requests to the `/create-volumes` and
`/destroy-volumes` endpoints, respectively. This capability is intended for use
by operators and administrative tools.

### `/create-volumes`

To use this endpoint, the operator should first ensure that a reservation for
the necessary resources has been made on the appropriate slave (e.g., by using
the [/reserve](endpoints/master/reserve.md) HTTP endpoint or by configuring a
static reservation).

To create a 512MB persistent volume for the `ads` role on a dynamically reserved
disk resource, we can send an HTTP POST request to the master's
[/create-volumes](endpoints/master/create-volumes.md) endpoint like so:

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

The user receives one of the following HTTP responses:

* `200 OK`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to create the volumes.

A single `/create-volumes` request can create multiple persistent volumes, but
all of the volumes must be on the same slave.

Note that when `200 OK` is returned by this endpoint, it does __not__ mean that
the persistent volumes have been created successfully. Instead, this return code
indicates that the create operation has been validated successfully by the
master. The request is then forwarded asynchronously to the Mesos slave where
the reserved resources are located. That asynchronous message may not be
delivered or creating the volumes at the slave might fail, in which case no
volumes will be created. To determine if a create operation has succeeded, the
user can examine the state of the appropriate Mesos slave (e.g., via the slave's
[/state](endpoints/slave/state.md) HTTP endpoint).

### `/destroy-volumes`

To destroy the volume created above, we can send an HTTP POST to the master's
[/destroy-volumes](endpoints/master/destroy-volumes.md) endpoint like so:

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

The user receives one of the following HTTP responses:

* `200 OK`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to destroy the volumes.

A single `/destroy-volumes` request can destroy multiple persistent volumes, but
all of the volumes must be on the same slave.

Note that when `200 OK` is returned by this endpoint, it does __not__ mean that
the persistent volumes have been destroyed successfully. Instead, this return
code indicates that the destroy operation has been validated successfully by the
master. The request is then forwarded asynchronously to the Mesos slave where
the persistent volumes are located. That asynchronous message may not be
delivered or destroying the volumes at the slave might fail, in which case no
volumes will be destroyed. To determine if a destroy operation has succeeded,
the user can examine the state of the appropriate Mesos slave (e.g., via the
slave's [/state](endpoints/slave/state.md) HTTP endpoint).

## Listing Persistent Volumes

Information about the persistent volumes at each slave in the cluster can be
found by querying the [/slaves](endpoints/master/slaves.md) master endpoint
(under the `reserved_resources_full` key).

## Programming with Persistent Volumes

Some suggestions to keep in mind when building applications that use persistent
volumes:

* A single `acceptOffers` call can be used to both create a new dynamic
  reservation (via `Offer::Operation::Reserve`) and create a new persistent
  volume on those newly reserved resources (via `Offer::Operation::Create`).

* Attempts to dynamically reserve resources or create persistent volumes might
  fail---for example, because the network message containing the operation did
  not reach the master or because the master rejected the operation.
  Applications should be prepared to detect failures and correct for them (e.g.,
  by retrying the operation).

* When using HTTP endpoints to reserve resources or create persistent volumes,
  _some_ failures can be detected by examining the HTTP response code returned
  to the client. However, it is still possible for a `200` response code to be
  returned to the client but for the associated operation to fail---see
  discussion above.

* When using the scheduler API, detecting that a dynamic reservation has failed
  is a little tricky: reservations do not have unique identifiers, and the Mesos
  master does not provide explicit feedback on whether a reservation request has
  succeeded or failed. Hence, framework schedulers typically use a combination
  of two techniques:

  1. They use timeouts to detect that a reservation request may have failed
     (because they don't receive a resource offer containing the expected
     resources after a given period of time).

  2. To check whether a resource offer includes the effect of a dynamic
     reservation, applications _cannot_ check for the presence of a "reservation
     ID" or similar value (because reservations do not have IDs). Instead,
     applications should examine the resource offer and check it contains
     sufficient reserved resources for the application's role. If it does not,
     the application should make additional reservation requests as necessary.

* When a scheduler issues a dynamic reservation request, the reserved resources
  might _not_ be present in the next resource offer the scheduler receives.
  There are two reasons for this: first, the reservation request might fail or
  be dropped by the network, as discussed above. Second, the reservation request
  might simply be delayed, so that the next resource offer from the master will
  be issued before the reservation request is received by the master. This is
  why the text above suggests that applications wait for a timeout before
  assuming that a reservation request should be retried.

* A consequence of using timeouts to detect failures is that an application
  might submit more reservation requests than intended (e.g., a timeout fires
  and an application makes another reservation request; meanwhile, the original
  reservation request is also processed). Recall that two reservations for the
  same role at the same slave are "merged": for example, role `foo` makes two
  requests to reserve 2 CPUs at a single slave and both reservation requests
  succeed, the result will be a single reservation of 4 CPUs. To handle this
  situation, applications should be prepared for resource offers that contain
  more resources than expected. Some applications may also want to detect this
  situation and unreserve any additional reserved resources that will not be
  required.

* It often makes sense to structure application logic as a "state machine",
  where the application moves from its initial state (no reserved resources and
  no persistent volumes) and eventually transitions toward a single terminal
  state (necessary resources reserved and persistent volume created). As new
  events (such as timeouts and resource offers) are received, the application
  compares the event with its current state and decides what action to take
  next.

* Because persistent volumes are associated with roles, a volume might be
  offered to _any_ of the frameworks that are registered in that role. For
  example, a persistent volume might be created by one framework and then
  offered to a different framework in the same role. This can be used to pass
  large volumes of data between frameworks in a convenient way. However, this
  behavior might also allow sensitive data created by one framework to be read
  or modified by another framework in the same role. It can also make it more
  difficult for frameworks to determine whether a dynamic reservation has
  succeeded: as discussed above, frameworks need to wait for an offer that
  contains the "expected" reserved resources to determine when a reservation
  request has succeeded. Determining what a framework should "expect" to find in
  an offer is more difficult when multiple frameworks can make reservations
  for the same role concurrently. In general, whenever multiple frameworks are
  allowed to register in the same role, the operator should ensure that those
  frameworks are configured to collaborate with one another when using
  role-specific resources. For more information, see the discussion of
  [multiple frameworks in the same role](roles.md#roles-multiple-frameworks).

## Version History

Persistent volumes were introduced in Mesos 0.23. Mesos 0.27 introduced HTTP
endpoints for creating and destroying volumes. Mesos 0.28 introduced support for
[multiple disk resources](multiple-disk.md), and also enhanced the `/slaves`
master endpoint to include detailed information about persistent volumes and
dynamic reservations. Mesos 0.29 changed the semantics of destroying a volume:
in previous releases, destroying a volume would remove the Mesos-level metadata,
but would not remove the volume's data from the slave's filesystem.
