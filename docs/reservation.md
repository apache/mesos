---
title: Apache Mesos - Reservation
layout: documentation
---

# Reservation

Mesos provides mechanisms to __reserve__ resources in specific slaves.
The concept was first introduced with __static reservation__ in 0.14.0
which enabled operators to specify the reserved resources on slave startup.
This was extended with __dynamic reservation__ in 0.23.0 which enabled operators
and authorized __frameworks__ to dynamically reserve resources in the cluster.

In both types of reservations, resources are reserved for a [__role__](roles.md).


## Static Reservation

An operator can configure a slave with resources reserved for a role.
The reserved resources are specified via the `--resources` flag.
For example, suppose we have 12 CPUs and 6144 MB of RAM available on a slave and
that we want to reserve 8 CPUs and 4096 MB of RAM for the `ads` role.
We start the slave like so:

        $ mesos-slave \
          --master=<ip>:<port> \
          --resources="cpus:4;mem:2048;cpus(ads):8;mem(ads):4096"

We now have 8 CPUs and 4096 MB of RAM reserved for `ads` on this slave.

__CAVEAT:__ In order to modify a static reservation, the operator must drain and
            restart the slave with the new configuration specified in the
            `--resources` flag.

__NOTE:__ This feature is supported for backwards compatibility.
          The recommended approach is to specify the total resources available
          on the slave as unreserved via the `--resources` flag and manage
          reservations dynamically via the master HTTP endpoints.


## Dynamic Reservation

As mentioned in [Static Reservation](#static-reservation-since-0140), specifying
the reserved resources via the `--resources` flag makes the reservation static.
That is, statically reserved resources cannot be reserved for another role nor
be unreserved. Dynamic reservation enables operators and authorized frameworks
to reserve and unreserve resources after slave-startup.

By default, frameworks and operators can reserve resources for any role, and can
unreserve any dynamically reserved resources. [Authorization](authorization.md)
allows this behavior to be limited so that only particular roles can be reserved
for, and only particular resources can be unreserved. For these operations to be
authorized, the framework or operator should provide a `principal` to identify
itself. To use authorization with reserve/unreserve operations, the Mesos master
must be configured with the appropriate ACLs. For more information, see the
[authorization documentation](authorization.md).

* `Offer::Operation::Reserve` and `Offer::Operation::Unreserve` messages are
  available for __frameworks__ to send back via the `acceptOffers` API as a
  response to a resource offer.
* `/reserve` and `/unreserve` HTTP endpoints allow __operators__ to manage
  dynamic reservations through the master.

In the following sections, we will walk through examples of each of the
interfaces described above.

If two dynamic reservations are made for the same role at a single slave (using
the same labels, if any; see below), the reservations will be combined by adding
together the resources reserved by each request. This will result in a single
reserved resource at the slave. Similarly, "partial" unreserve operations are
allowed: an unreserve operation can release some but not all of the resources at
a slave that have been reserved for a role. In this case, the unreserved
resources will be subtracted from the previous reservation and any remaining
resources will still be reserved.

Dynamic reservations cannot be unreserved if they are still being used by a
running task or if a [persistent volume](persistent-volume.md) has been created
using the reserved resources. In the latter case, the volume should be destroyed
before unreserving the resources.

### Labeled Reservations

Dynamic reservations can optionally include a list of _labels_, which are
arbitrary key-value pairs. Labels can be used to associate arbitrary metadata
with a resource reservation. For example, frameworks can use labels to identify
the intended purpose for a portion of the resources that have been reserved at a
given slave. Note that two reservations with different labels will not be
combined together into a single reservation, even if the reservations are at the
same slave and use the same role.


### Framework Scheduler API

#### `Offer::Operation::Reserve`

A framework can reserve resources through the resource offer cycle.  Suppose we
receive a resource offer with 12 CPUs and 6144 MB of RAM unreserved.

        {
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 12 },
              "role": "*",
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 6144 },
              "role": "*",
            }
          ]
        }

We can reserve 8 CPUs and 4096 MB of RAM by sending the following
`Offer::Operation` message. `Offer::Operation::Reserve` has a `resources` field
which we specify with the resources to be reserved. We need to explicitly set
the `role` and `principal` fields with the framework's role and principal.

        {
          "type": Offer::Operation::RESERVE,
          "reserve": {
            "resources": [
              {
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "role": <framework_role>,
                "reservation": {
                  "principal": <framework_principal>
                }
              },
              {
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "role": <framework_role>,
                "reservation": {
                  "principal": <framework_principal>
                }
              }
            ]
          }
        }

If the reservation is successful, a subsequent resource offer will contain the
following reserved resources:

        {
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": <framework_role>,
              "reservation": {
                "principal": <framework_principal>
              }
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": <framework_role>,
              "reservation": {
                "principal": <framework_principal>
              }
            },
          ]
        }


#### `Offer::Operation::Unreserve`

A framework can unreserve resources through the resource offer cycle.
In [Offer::Operation::Reserve](#offeroperationreserve), we reserved 8 CPUs
and 4096 MB of RAM on a particular slave for our `role`. The master will
continue to only offer these resources to our `role`. Suppose we would like to
unreserve these resources. First, we receive a resource offer (copy/pasted
from above):

        {
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": <framework_role>,
              "reservation": {
                "principal": <framework_principal>
              }
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": <framework_role>,
              "reservation": {
                "principal": <framework_principal>
              }
            },
          ]
        }

We can unreserve the 8 CPUs and 4096 MB of RAM by sending the following
`Offer::Operation` message. `Offer::Operation::Unreserve` has a `resources` field
which we can use to specify the resources to be unreserved.

        {
          "type": Offer::Operation::UNRESERVE,
          "unreserve": {
            "resources": [
              {
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "role": <framework_role>,
                "reservation": {
                  "principal": <framework_principal>
                }
              },
              {
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "role": <framework_role>,
                "reservation": {
                  "principal": <framework_principal>
                }
              }
            ]
          }
        }

The unreserved resources may now be offered to other frameworks.

### Operator HTTP Endpoints

As described above, dynamic reservations can be made by a framework scheduler,
typically in response to a resource offer. However, dynamic reservations can
also be created and deleted by sending HTTP requests to the `/reserve` and
`/unreserve` endpoints, respectively. This capability is intended for use by
operators and administrative tools.

#### `/reserve` (since 0.25.0)

Suppose we want to reserve 8 CPUs and 4096 MB of RAM for the `ads` role on a
slave with id=`<slave_id>` (note that it is up to the user to find the ID of the
slave that hosts the desired resources; the request will fail if sufficient
unreserved resources cannot be found on the slave). In this case, the principal
included in the request will be the principal of an authorized operator rather
than the principal of a framework registered under the `ads` role. We send an
HTTP POST request to the master's [/reserve](endpoints/master/reserve.md)
endpoint like so:

        $ curl -i \
          -u <operator_principal>:<password> \
          -d slaveId=<slave_id> \
          -d resources='[
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": "ads",
              "reservation": {
                "principal": <operator_principal>
              }
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": "ads",
              "reservation": {
                "principal": <operator_principal>
              }
            }
          ]' \
          -X POST http://<ip>:<port>/master/reserve

The user receives one of the following HTTP responses:

* `200 OK`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the reserve operation.

Note that when `200 OK` is returned by this endpoint, it does __not__ mean that
the requested resources have been reserved. Instead, this return code indicates
that the reservation request has been validated successfully by the master. The
reservation request is then forwarded asynchronously to the Mesos slave where
the resources are located. That asynchronous message may not be delivered, in
which case no resources will be reserved. To determine if a reserve operation
has succeeded, the user can examine the state of the appropriate Mesos slave
(e.g., via the slave's [/state](endpoints/slave/state.md) HTTP endpoint).

#### `/unreserve` (since 0.25.0)

Suppose we want to unreserve the resources that we dynamically reserved above.
We can send an HTTP POST request to the master's
[/unreserve](endpoints/master/unreserve.md) endpoint like so:

        $ curl -i \
          -u <operator_principal>:<password> \
          -d slaveId=<slave_id> \
          -d resources='[
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": "ads",
              "reservation": {
                "principal": <reserver_principal>
              }
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": "ads",
              "reservation": {
                "principal": <reserver_principal>
              }
            }
          ]' \
          -X POST http://<ip>:<port>/master/unreserve

Note that `reserver_principal` is the principal that was used to make the
reservation, while `operator_principal` is the principal that is attempting to
perform the unreserve operation---in some cases, these principals might be the
same. The `operator_principal` must be [authorized](authorization.md) to
unreserve reservations made by `reserver_principal`.

The user receives one of the following HTTP responses:

* `200 OK`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the unreserve operation.

Note that when `200 OK` is returned by this endpoint, it does __not__ mean that
the requested resources have been unreserved. Instead, this return code
indicates that the unreserve request has been validated successfully by the
master. The request is then forwarded asynchronously to the Mesos slave where
the resources are located. That asynchronous message may not be delivered, in
which case no resources will be unreserved. To determine if an unreserve
operation has succeeded, the user can examine the state of the appropriate Mesos
slave (e.g., via the slave's [/state](endpoints/slave/state.md) HTTP endpoint).

### Listing Reservations

Information about the reserved resources at each slave in the cluster can be
found by querying the [/slaves](endpoints/master/slaves.md) master endpoint
(under the `reserved_resources_full` key).
