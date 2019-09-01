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


<a name="static-reservation"></a>
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

It's often more convenient to specify the total resources available on
the slave as unreserved via the `--resources` flag and manage reservations
dynamically (see below) via the master HTTP endpoints. However static
reservation provides a way for the operator to more deterministically control
the reservations (roles, amount, principals) before the agent is exposed to the
master and frameworks. One use case is for the operator to dedicate entire
agents for specific roles.


## Dynamic Reservation

As mentioned in [Static Reservation](#static-reservation), specifying
the reserved resources via the `--resources` flag makes the reservation static.
That is, statically reserved resources cannot be reserved for another role nor
be unreserved. Dynamic reservation enables operators and authorized frameworks
to reserve and unreserve resources after slave-startup.

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

## Authorization

By default, frameworks and operators are authorized to reserve resources for
any role and to unreserve dynamically reserved resources.
[Authorization](authorization.md) allows this behavior to be limited so that
resources can only be reserved for particular roles, and only particular
resources can be unreserved. For these operations to be authorized, the
framework or operator should provide a `principal` to identify itself. To use
authorization with reserve/unreserve operations, the Mesos master must be
configured with the appropriate ACLs. For more information, see the
[authorization documentation](authorization.md).

Similarly, agents by default can register with the master with resources that
are statically reserved for arbitrary roles.
With [authorization](authorization.md),
the master can be configured to use the `reserve_resources` ACL to check that
the agent's `principal` is allowed to statically reserve resources for specific
roles.

## Reservation Labels

Dynamic reservations can optionally include a list of _labels_, which are
arbitrary key-value pairs. Labels can be used to associate arbitrary metadata
with a resource reservation. For example, frameworks can use labels to identify
the intended purpose for a portion of the resources that have been reserved at a
given slave. Note that two reservations with different labels will not be
combined together into a single reservation, even if the reservations are at the
same slave and use the same role.

## Reservation Refinement

Hierarhical roles such as `eng/backend` enable the delegation of resources
down a hierarchy, and reservation refinement is the mechanism with which
__reservations__ are delegated down the hierarchy. For example, a reservation
(static or dynamic) for `eng` can be __refined__ to `eng/backend`. When such
a reservation is unreserved, they are returned to the previous owner. In this
case it would be returned to `eng`. Reservation refinements can also "skip"
levels. For example, `eng` can be __refined__ directly to `eng/backend/db`.
Again, unreserving such a reservation is returned to its previous owner `eng`.

**NOTE:** Frameworks need to enable the `RESERVATION_REFINEMENT` capability
in order to be offered, and to create refined reservations

## Listing Reservations

Information about the reserved resources at each slave in the cluster can be
found by querying the [/slaves](endpoints/master/slaves.md) master endpoint
(under the `reserved_resources_full` key).

The same information can also be found in the [/state](endpoints/slave/state.md)
endpoint on the agent (under the `reserved_resources_full` key). The agent
endpoint is useful to confirm if a reservation has been propagated to the
agent (which can fail in the event of network partition or master/agent
restarts).

## Examples

### Framework Scheduler API

<a name="offer-operation-reserve"></a>
#### `Offer::Operation::Reserve` (__without__ `RESERVATION_REFINEMENT`)

A framework can reserve resources through the resource offer cycle. The
reservation role must match the offer's allocation role. Suppose we
receive a resource offer with 12 CPUs and 6144 MB of RAM unreserved, allocated
to role `"engineering"`.

        {
          "allocation_info": { "role": "engineering" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 12 },
              "role": "*",
            },
            {
              "allocation_info": { "role": "engineering" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 6144 },
              "role": "*",
            }
          ]
        }

We can reserve 8 CPUs and 4096 MB of RAM by sending the following
`Offer::Operation` message. `Offer::Operation::Reserve` has a `resources` field
which we specify with the resources to be reserved. We must explicitly set the
resources' `role` field to the offer's allocation role. The required value of
the `principal` field depends on whether or not the framework provided a
principal when it registered with the master. If a principal was provided, then
the resources' `principal` field must be equal to the framework's principal.
If no principal was provided during registration, then the resources'
`principal` field can take any value, or can be left unset. Note that the
`principal` field determines the "reserver principal" when
[authorization](authorization.md) is enabled, even if authentication is
disabled.

        {
          "type": Offer::Operation::RESERVE,
          "reserve": {
            "resources": [
              {
                "allocation_info": { "role": "engineering" },
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "role": "engineering",
                "reservation": {
                  "principal": <framework_principal>
                }
              },
              {
                "allocation_info": { "role": "engineering" },
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "role": "engineering",
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
          "allocation_info": { "role": "engineering" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": "engineering",
              "reservation": {
                "principal": <framework_principal>
              }
            },
            {
              "allocation_info": { "role": "engineering" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": "engineering",
              "reservation": {
                "principal": <framework_principal>
              }
            },
          ]
        }


#### `Offer::Operation::Unreserve` (__without__ `RESERVATION_REFINEMENT`)

A framework can unreserve resources through the resource offer cycle.
In [Offer::Operation::Reserve](#offer-operation-reserve), we reserved 8 CPUs
and 4096 MB of RAM on a particular slave for one of our subscribed roles
(e.g. `"engineering"`). The master will continue to only offer these reserved
resources to the reservation's `role`. Suppose we would like to unreserve
these resources. First, we receive a resource offer (copy/pasted from above):

        {
          "allocation_info": { "role": "engineering" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "role": "engineering",
              "reservation": {
                "principal": <framework_principal>
              }
            },
            {
              "allocation_info": { "role": "engineering" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "role": "engineering",
              "reservation": {
                "principal": <framework_principal>
              }
            },
          ]
        }

We can unreserve the 8 CPUs and 4096 MB of RAM by sending the following
`Offer::Operation` message. `Offer::Operation::Unreserve` has a `resources`
field which we can use to specify the resources to be unreserved.

        {
          "type": Offer::Operation::UNRESERVE,
          "unreserve": {
            "resources": [
              {
                "allocation_info": { "role": "engineering" },
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "role": "engineering",
                "reservation": {
                  "principal": <framework_principal>
                }
              },
              {
                "allocation_info": { "role": "engineering" },
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "role": "engineering",
                "reservation": {
                  "principal": <framework_principal>
                }
              }
            ]
          }
        }

The unreserved resources may now be offered to other frameworks.

<a name="offer-operation-reserve-reservation-refinement"></a>
#### `Offer::Operation::Reserve` (__with__ `RESERVATION_REFINEMENT`)

A framework that wants to create a refined reservation needs to enable
the `RESERVATION_REFINEMENT` capability. Doing so will allow the framework
to use the `reservations` field in the `Resource` message in order to
__push__ a refined reservation.

Since reserved resources are offered to any of the child roles under the role
for which they are reserved for, they can get __allocated__ to say,
`"engineering/backend"` while being __reserved__ for `"engineering"`.
It can then be refined to be __reserved__ for `"engineering/backend"`.

Note that the refined reservation role must match the offer's allocation role.

Suppose we receive a resource offer with 12 CPUs and 6144 MB of RAM reserved to
`"engineering"`, allocated to role `"engineering/backend"`.

        {
          "allocation_info": { "role": "engineering/backend" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 12 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                }
              ]
            },
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 6144 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                }
              ]
            }
          ]
        }

Take note of the fact that `role` and `reservation` are not set, and that there
is a new field called `reservations` which represents the reservation state.
With `RESERVATION_REFINEMENT` enabled, the framework receives resources in this
new format where solely the `reservations` field is used for the reservation
state, rather than `role`/`reservation` pair from pre-`RESERVATION_REFINEMENT`.

We can reserve 8 CPUs and 4096 MB of RAM to `"engineering/backend"` by sending
the following `Offer::Operation` message. `Offer::Operation::Reserve` has
a `resources` field which we specify with the resources to be reserved.
We must __push__ a new `ReservationInfo` message onto the back of
the `reservations` field. We must explicitly set the reservation's' `role` field
to the offer's allocation role. The optional value of the `principal` field
depends on whether or not the framework provided a principal when it registered
with the master. If a principal was provided, then the resources' `principal`
field must be equal to the framework's principal. If no principal was provided
during registration, then the resources' `principal` field can take any value,
or can be left unset.  Note that the `principal` field determines
the "reserver principal" when [authorization](authorization.md) is enabled, even
if authentication is disabled.

        {
          "type": Offer::Operation::RESERVE,
          "reserve": {
            "resources": [
              {
                "allocation_info": { "role": "engineering/backend" },
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "reservations": [
                  {
                    "type": "DYNAMIC",
                    "role": "engineering",
                    "principal": <principal>,
                  },
                  {
                    "type": "DYNAMIC",
                    "role": "engineering/backend",
                    "principal": <framework_principal>,
                  }
                ]
              },
              {
                "allocation_info": { "role": "engineering/backend" },
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "reservations": [
                  {
                    "type": "DYNAMIC",
                    "role": "engineering",
                    "principal": <principal>,
                  },
                  {
                    "type": "DYNAMIC",
                    "role": "engineering/backend",
                    "principal": <framework_principal>,
                  }
                ]
              }
            ]
          }
        }

If the reservation is successful, a subsequent resource offer will contain the
following reserved resources:

        {
          "allocation_info": { "role": "engineering/backend" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                },
                {
                  "type": "DYNAMIC",
                  "role": "engineering/backend",
                  "principal": <framework_principal>,
                }
              ]
            },
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                },
                {
                  "type": "DYNAMIC",
                  "role": "engineering/backend",
                  "principal": <framework_principal>,
                }
              ]
            },
          ]
        }


#### `Offer::Operation::Unreserve` (__with__ `RESERVATION_REFINEMENT`)

A framework can unreserve resources through the resource offer cycle.
In [Offer::Operation::Reserve](#offer-operation-reserve-reservation-refinement),
we reserved 8 CPUs and 4096 MB of RAM on a particular slave for one of our
subscribed roles (i.e. `"engineering/backend"`), previously reserved for
`"engineering"`. When we unreserve these resources, they are returned to
`"engineering"`, by the last `ReservationInfo` added to
the `reservations` field being __popped__. First, we receive a resource offer
(copy/pasted from above):

        {
          "allocation_info": { "role": "engineering/backend" },
          "id": <offer_id>,
          "framework_id": <framework_id>,
          "slave_id": <slave_id>,
          "hostname": <hostname>,
          "resources": [
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                },
                {
                  "type": "DYNAMIC",
                  "role": "engineering/backend",
                  "principal": <framework_principal>,
                }
              ]
            },
            {
              "allocation_info": { "role": "engineering/backend" },
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "engineering",
                  "principal": <principal>,
                },
                {
                  "type": "DYNAMIC",
                  "role": "engineering/backend",
                  "principal": <framework_principal>,
                }
              ]
            },
          ]
        }


We can unreserve the 8 CPUs and 4096 MB of RAM by sending the following
`Offer::Operation` message. `Offer::Operation::Unreserve` has a `resources`
field which we can use to specify the resources to be unreserved.

        {
          "type": Offer::Operation::UNRESERVE,
          "unreserve": {
            "resources": [
              {
                "allocation_info": { "role": "engineering/backend" },
                "name": "cpus",
                "type": "SCALAR",
                "scalar": { "value": 8 },
                "reservations": [
                  {
                    "type": "DYNAMIC",
                    "role": "engineering",
                    "principal": <principal>,
                  },
                  {
                    "type": "DYNAMIC",
                    "role": "engineering/backend",
                    "principal": <framework_principal>,
                  }
                ]
              },
              {
                "allocation_info": { "role": "engineering/backend" },
                "name": "mem",
                "type": "SCALAR",
                "scalar": { "value": 4096 },
                "reservations": [
                  {
                    "type": "DYNAMIC",
                    "role": "engineering",
                    "principal": <principal>,
                  },
                  {
                    "type": "DYNAMIC",
                    "role": "engineering/backend",
                    "principal": <framework_principal>,
                  }
                ]
              },
            ]
          }
        }

The resources will now be reserved for `"engineering"` again, and may now be
offered to `"engineering"` role itself, or other roles under `"engineering"`.

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
that must be included in the `reservation` field of the reserved resources
depends on the status of HTTP authentication on the master. If HTTP
authentication is enabled, then the principal in the reservation should match
the authenticated principal provided in the request's HTTP headers. If HTTP
authentication is disabled, then the principal in the reservation can take any
value, or can be left unset. Note that the `principal` field determines the
"reserver principal" when [authorization](authorization.md) is enabled, even if
HTTP authentication is disabled.

We send an HTTP POST request to the master's
[/reserve](endpoints/master/reserve.md) endpoint like so:

        $ curl -i \
          -u <operator_principal>:<password> \
          -d slaveId=<slave_id> \
          -d resources='[
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 8 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "ads",
                  "principal": <operator_principal>,
                }
              ]
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "ads",
                  "principal": <operator_principal>,
                }
              ]
            }
          ]' \
          -X POST http://<ip>:<port>/master/reserve

The user receives one of the following HTTP responses:

* `202 Accepted`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the reserve operation.

This endpoint returns the 202 ACCEPTED HTTP status code, which indicates that
the reserve operation has been validated successfully by the master. The
request is then forwarded asynchronously to the Mesos slave where the reserved
resources are located. That asynchronous message may not be delivered or
reserving resources at the slave might fail, in which case no resources will be
reserved. To determine if a reserve operation has succeeded, the user can
examine the state of the appropriate Mesos slave (e.g., via the slave's
[/state](endpoints/slave/state.md) HTTP endpoint).

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
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "ads",
                  "principal": <reserver_principal>,
                }
              ]
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 4096 },
              "reservations": [
                {
                  "type": "DYNAMIC",
                  "role": "ads",
                  "principal": <reserver_principal>,
                }
              ]
            }
          ]' \
          -X POST http://<ip>:<port>/master/unreserve

Note that `reserver_principal` is the principal that was used to make the
reservation, while `operator_principal` is the principal that is attempting to
perform the unreserve operation---in some cases, these principals might be the
same. The `operator_principal` must be [authorized](authorization.md) to
unreserve reservations made by `reserver_principal`.

The user receives one of the following HTTP responses:

* `202 Accepted`: Request accepted (see below).
* `400 BadRequest`: Invalid arguments (e.g., missing parameters).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: Insufficient resources to satisfy the unreserve operation.

This endpoint returns the 202 ACCEPTED HTTP status code, which indicates that
the unreserve operation has been validated successfully by the master. The
request is then forwarded asynchronously to the Mesos slave where the reserved
resources are located. That asynchronous message may not be delivered or
unreserving resources at the slave might fail, in which case no resources will
be unreserved. To determine if an unreserve operation has succeeded, the user
can examine the state of the appropriate Mesos slave (e.g., via the slave's
[/state](endpoints/slave/state.md) HTTP endpoint).
