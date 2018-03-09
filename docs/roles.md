---
title: Apache Mesos - Roles
layout: documentation
---

# Roles

Many modern host-level operating systems (e.g. Linux, BSDs, etc) support
multiple users. Similarly, Mesos is a multi-user cluster management system,
with the expectation of a single Mesos cluster managing an organization's
resources and servicing the organization's users.

As such, Mesos has to address a number of requirements related to resource
management:

* Fair sharing of the resources amongst users
* Providing resource guarantees to users (e.g. quota, priorities, isolation)
* Providing accurate resource accounting
    * How many resources are allocated / utilized / etc?
    * Per-user accounting

In Mesos, we refer to these "users" as __roles__. More precisely, a __role__
within Mesos refers to a resource consumer within the cluster. This resource
consumer could represent a user within an organization, but it could also
represent a team, a group, a service, a framework, etc.

Schedulers subscribe to one or more roles in order to receive resources and
schedule work on behalf of the resource consumer(s) they are servicing.

Some examples of resource allocation guarantees that Mesos provides:

* Guaranteeing that a role is allocated a specified amount of resources
  (via [quota](quota.md)).
* Ensuring that some (or all) of the resources on a particular agent
  are allocated to a particular role (via [reservations](reservation.md)).
* Ensuring that resources are fairly shared between roles
  (via [DRF](https://www.cs.berkeley.edu/~alig/papers/drf.pdf)).
* Expressing that some roles should receive a higher relative share of the
  cluster (via [weights](weights.md)).

## Roles and access control

There are two ways to control which roles a framework is allowed to subscribe
to. First, ACLs can be used to specify which framework principals can subscribe
to which roles. For more information, see the [authorization](authorization.md)
documentation.

Second, a _role whitelist_ can be configured by passing the `--roles` flag to
the Mesos master at startup. This flag specifies a comma-separated list of role
names. If the whitelist is specified, only roles that appear in the whitelist
can be used. To change the whitelist, the Mesos master must be restarted. Note
that in a high-availability deployment of Mesos, you should take care to ensure
that all Mesos masters are configured with the same whitelist.

In Mesos 0.26 and earlier, you should typically configure _both_ ACLs and the
whitelist, because in these versions of Mesos, any role that does not appear in
the whitelist cannot be used.

In Mesos 0.27, this behavior has changed: if `--roles` is not specified, the
whitelist permits _any role name_ to be used. Hence, in Mesos 0.27, the
recommended practice is to only use ACLs to define which roles can be used; the
`--roles` command-line flag is deprecated.

## Associating frameworks with roles

A framework specifies which roles it would like to subscribe to when it
subscribes with the master. This is done via the `roles` field in
`FrameworkInfo`. A framework can also change which roles it is
subscribed to by reregistering with an updated `FrameworkInfo`.

As a user, you can typically specify which role(s) a framework will
subscribe to when you start the framework. How to do this depends on the
user interface of the framework you're using. For example, a single user
scheduler might take a `--mesos_role` command-line flag and a multi-user
scheduler might take a `--mesos-roles` command-line flag or sync with
the organization's LDAP system to automatically adjust which roles it is
subscribed to as the organization's structure changes.

### Subscribing to multiple roles

As noted above, a framework can subscribe to multiple roles
simultaneously. Frameworks that want to do this must opt-in to the
`MULTI_ROLE` capability.

When a framework is offered resources, those resources are associated
with exactly _one_ of the roles it has subscribed to; the framework can
determine which role an offer is for by consulting the
`allocation_info.role` field in the `Offer` or the
`allocation_info.role` field in each offered `Resource` (in the current
implementation, all the resources in a single `Offer` will be allocated
to the same role).

<a id="roles-multiple-frameworks"></a>
### Multiple frameworks in the same role

Multiple frameworks can be subscribed to the same role. This can be useful:
for example, one framework can create a persistent volume and write data to
it. Once the task that writes data to the persistent volume has finished,
the volume will be offered to other frameworks subscribed to the same role;
this might give a second ("consumer") framework the opportunity to launch a
task that reads the data produced by the first ("producer") framework.

However, configuring multiple frameworks to use the same role should be done
with caution, because all the frameworks will have access to any resources that
have been reserved for that role. For example, if a framework stores sensitive
information on a persistent volume, that volume might be offered to a different
framework subscribed to the same role. Similarly, if one framework creates a
persistent volume, another framework subscribed to the same role might "steal"
the volume and use it to launch a task of its own. In general, multiple
frameworks sharing the same role should be prepared to collaborate with one
another to ensure that role-specific resources are used appropriately.

## Associating resources with roles

A resource is assigned to a role using a _reservation_. Resources can either be
reserved _statically_ (when the agent that hosts the resource is started) or
_dynamically_: frameworks and operators can specify that a certain resource
should subsequently be reserved for use by a given role. For more information,
see the [reservation](reservation.md) documentation.

## Default role

The role named `*` is special. Unreserved resources are currently represented
as having the special `*` role (the idea being that `*` matches any role). By
default, all the resources at an agent node are unreserved (this can be changed
via the `--default_role` command-line flag when starting the agent).

In addition, when a framework registers without providing a
`FrameworkInfo.role`, it is assigned to the `*` role. In Mesos 1.3, frameworks
should use the `FrameworkInfo.roles` field, which does not assign a default of
`*`, but frameworks can still specify `*` explicitly if desired. Frameworks
and operators cannot make reservations to the `*` role.

## Invalid role names

A role name must be a valid directory name, so it cannot:

* Be an empty string
* Be `.` or `..`
* Start with `-`
* Contain any slash, backspace, or whitespace character

## Roles and resource allocation

By default, the Mesos master uses weighted Dominant Resource Fairness (wDRF) to
allocate resources. In particular, this implementation of wDRF first identifies
which _role_ is furthest below its fair share of the role's dominant resource.
Each of the frameworks subscribed to that role are then offered additional
resources in turn.

The resource allocation process can be customized by assigning
_[weights](weights.md)_ to roles: a role with a weight of 2 will be allocated
twice the fair share of a role with a weight of 1. By default, every role has a
weight of 1. Weights can be configured using the
[/weights](endpoints/master/weights.md) operator endpoint, or else using the
deprecated `--weights` command-line flag when starting the Mesos master.

## Roles and quota

In order to guarantee that a role is allocated a specific amount of resources,
quota can be specified via the [/quota](endpoints/master/quota.md) endpoint.

The resource allocator will first attempt to satisfy the quota requirements,
before fairly sharing the remaining resources. For more information, see the
[quota](quota.md) documentation.

## Role vs. Principal

A principal identifies an entity that interacts with Mesos; principals are
similar to user names. For example, frameworks supply a principal when they
register with the Mesos master, and operators provide a principal when using the
operator HTTP endpoints. An entity may be required to
[authenticate](authentication.md) with its principal in order to prove its
identity, and the principal may be used to [authorize](authorization.md) actions
performed by an entity, such as [resource reservation](reservation.md) and
[persistent volume](persistent-volume.md) creation/destruction.

Roles, on the other hand, are used exclusively for resource allocation, as
covered above.
