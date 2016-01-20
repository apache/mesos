---
layout: documentation
---

# Reservation

Mesos provides mechanisms to __reserve__ resources in specific slaves.
The concept was first introduced with __static reservation__ in 0.14.0
which enabled operators to specify the reserved resources on slave startup.
This was extended with __dynamic reservation__ in 0.23.0 which enabled operators
and authorized __frameworks__ to dynamically reserve resources in the cluster.

In both types of reservations, resources are reserved for a [__role__](roles.md).


## Static Reservation (since 0.14.0)

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
            restart the slave with the new configuration specifed in the
            `--resources` flag.

__NOTE:__ This feature is supported for backwards compatibility.
          The recommended approach is to specify the total resources available
          on the slave as unreserved via the `--resources` flag and manage
          reservations dynamically via the master HTTP endpoints.


## Dynamic Reservation (since 0.23.0)

As mentioned in [Static Reservation](#static-reservation-since-0140), specifying
the reserved resources via the `--resources` flag makes the reservation static.
This is, statically reserved resources cannot be reserved for another role nor
be unreserved. Dynamic Reservation enables operators and authorized frameworks
to reserve and unreserve resources post slave-startup.

We require a `principal` from the operator or framework in order to
authenticate/authorize the operations. Permissions are specified via the
existing ACL mechanism. To use authorization with reserve/unreserve operations,
the Mesos master must be configured with the desired ACLs. For more information,
see the [authorization documentation](authorization.md).

* `Offer::Operation::Reserve` and `Offer::Operation::Unreserve` messages are
  available for __frameworks__ to send back via the `acceptOffers` API as a
  response to a resource offer.
* `/reserve` and `/unreserve` HTTP endpoints allow __operators__ to manage
  dynamic reservations through the master. NOTE: As of 0.27.0, these endpoints
  cannot be used when HTTP authentication is disabled due to the current
  implementation. This will change in version 0.28.0.

In the following sections, we will walk through examples of each of the
interfaces described above.

Note that if two dynamic reservations are made for resources at a single slave,
the reservations will be combined by adding together the resources reserved by
each request. Similarly, "partial" unreserve operations are allowed: an
unreserve operation can release only some of the resources at a slave that have
been reserved for a given role. In this case, the unreserved resources will be
subtracted from the previous reservation, and any remaining resources will still
be reserved.

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
which we specify with the resources to be reserved. We need to expicitly set
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
unreserved resources cannot be found on the slave). We send an HTTP POST request
to the `/reserve` HTTP endpoint like so:

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

* `200 OK`: Success (the requested resources have been reserved).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the reserve operation.

#### `/unreserve` (since 0.25.0)

Suppose we want to unreserve the resources that we dynamically reserved above.
We can send an HTTP POST request to the `/unreserve` HTTP endpoint like so:

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
          -X POST http://<ip>:<port>/master/unreserve

The user receives one of the following HTTP responses:

* `200 OK`: Success (the requested resources have been unreserved).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the unreserve operation.
