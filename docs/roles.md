---
layout: documentation
---

# Roles

In Mesos, __roles__ can be used to specify that certain
[resources](attributes-resources.md) are reserved for the use of one or more
frameworks. Roles can be used to enable a variety of restrictions on how
resources are offered to frameworks. Some use-cases for roles include:

* arranging for all the resources on a particular agent to only be offered to a
  particular framework.
* dividing a cluster between two organizations: resources assigned for use by
  organization _A_ will only be offered to that frameworks that have registered
  using organization _A_'s role.
* ensuring that [persistent volumes](persistent-volume.md) created by one
  framework are not offered to frameworks registered with a different role.
* expressing that one group of frameworks should be considered "higher priority"
  (and offered more resources) than another group of frameworks.

## Defining roles

The set of legal roles is configured statically, when the Mesos master is
started. The `--roles` command-line argument specifies a comma-separated list of
role names. To change the set of roles, the Mesos master must be restarted.

Note that you should take care to ensure that all Mesos masters are configured
to use the same set of roles.

## Associating frameworks with roles

A framework can optionally specify the role it would like to use when it
registers with the master.

As a developer, you can specify the role your framework will use via the `role`
field of the `FrameworkInfo` message.

As a user, you can typically specify which role a framework will use when you
start the framework. How to do this depends on the user interface of the
framework you're using; for example, Marathon takes a `--mesos_role`
command-line flag.

As an administrator, you can use ACLs to specify which framework principals can
register as which roles. For more information, see the
[authorization](authorization.md) documentation.

## Associating resources with roles

A resource is assigned to a role using a _reservation_. Resources can either be
reserved _statically_ (when the agent that hosts the resource is started) or
_dynamically_: frameworks and operators can specify that a certain resource
should subsequently be reserved for use by a given role. For more information,
see the [reservation](reservation.md) documentation.

## The default role

The role named `*` is special. Resources that are assigned to the `*` role are
considered "unreserved"; similarly, when a framework registers without providing
a role, it is assigned to the `*` role. By default, all the resources at an
agent node are initially assigned to the `*` role (this can be changed via the
`--default_role` command-line flag when starting the agent).

The `*` role behaves differently from non-default roles. For example, dynamic
reservations can be used to reassign resources from the `*` role to a specific
role, but not from one specific role to another specific role (without first
unreserving the resource, e.g., using the `/unreserve` operator HTTP
endpoint). Similarly, persistent volumes cannot be created on unreserved
resources.

## Roles and resource allocation

By default, the Mesos master uses Dominant Resource Fairness (DRF) to allocate
resources. In particular, this implementation of DRF first identifies which
_role_ is furthest below its fair share of the role's dominant resource. Each of
the frameworks in that role are then offered additional resources in turn.

The resource allocation process can be customized by assigning _weights_ to
roles: a role with a weight of 2 will be allocated twice the fair share of a
role with a weight of 1. Weights are optional, and can be specified via the
`--weights` command-line flag when starting the Mesos master.
