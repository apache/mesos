---
layout: documentation
---

# Quota

A problem when running multiple frameworks on Mesos is that the default
[wDRF allocator](https://www.cs.berkeley.edu/~alig/papers/drf.pdf) may offer
resources to frameworks even though they are beyond their fair share (e.g., when
no other framework currently accepts these resources). There is no mechanism to
lay away resources for future consumption in an entire cluster. Even though
[dynamic reservations](reservation.md) allow both operators and frameworks
to dynamically reserve resources on particular Mesos agents, it does not solve
the above problem because any individual agents might fail.

Since version 0.27.0 Mesos provides a mechanism to reserve a certain amount of
resources in an entire cluster, i.e. not tied to a particular agent. _Quota_
allows operators to lay away resources for a given [role](roles.md). These
resources cannot be hijacked by other roles and are guaranteed to be available
for the role if there are enough resources in the cluster. Quota can be viewed
as a cluster-wide dynamic reservation available for operators only.

# Terminology

For the purpose of this document, an “Operator” is a person, tool, or script
that manages the Mesos cluster.

In computer science, a “quota” usually refers to one of the following:
* A minimal guarantee.
* A maximal limit.
* A pair of both.

In Mesos we understand quota as a **guaranteed** resource allocation that a role
may rely on; in other words, a minimum share a role is entitled to receive.

# Motivation and Limitations

Consider the following scenarios in a Mesos cluster to better understand what
use-cases are supported by quota (and what are not).

## Scenario 1: Greedy Framework
There are two frameworks in a cluster, each running in a separate role with
equal weights: framework fA in role rA and framework fB in role rB. There is a
single resource available in the cluster: 100 CPUs. fA consumes 10 CPUs and is
idle (declines resource offers), while fB is greedy and accepts all offers it gets,
hogging the remaining 90 CPUs. Without quota, though fA’s fair share is 50 CPUs
it will not be able to make use of additional the 40 CPUs until some of fB’s
tasks terminate.

## Scenario 2: Resources for a new Framework
A greedy framework fB in role rB is currently the only framework in the cluster
and it uses all available resources---100 CPUs. If a new framework fA in role rA
joins the cluster, it will not receive its fair share of the cluster resources
(50 CPUs) until some of fB’s tasks terminate.

To deal with Scenario 2, quota by itself is not a sufficient solution as it
would be set after fB has started using all resources. Instead Scenario 2
requires either always keeping a pool of resources which are not offered or
introducing preemption for running tasks.

# Operator HTTP Endpoint

The master `/quota` HTTP endpoint enables operators to configure quotas. The
endpoint currently offers a REST-like interface and supports the
following operations:

* [Setting](#setRequest) a new quota with POST.
* [Removing](#removeRequest) an existing quota with DELETE.
* [Querying](#statusRequest) the currently set quota with GET.

Currently it is not possible to update previously configured quotas. This means
in order to update a quota for a given role, the operator has to remove the
existing quota and then set a new one.

The endpoint can optionally use authentication and authorization. See the
[authentication guide](authentication.md) for details.

<a name="setRequest"></a>
## Set

The operator can set a new quota by sending an HTTP POST request to the `/quota`
endpoint.

An example request to the quota endpoint could look like this (using the JSON
definitions below):

    $ curl -d jsonMessageBody -X POST http://<master-ip>:<port>/quota

For example to set a quota of 12 CPUs and 6144 MB of RAM for `role1` the operator
can use the following `jsonMessageBody`:

        {
          "role": "role1",
          "guarantee": [
            {
              "name": "cpus",
              "type": "SCALAR",
              "scalar": { "value": 12 }
            },
            {
              "name": "mem",
              "type": "SCALAR",
              "scalar": { "value": 6144 }
            }
          ]
        }

A set request is only valid for roles for which no quota is currently set.
However if the master is configured without an explicit
[role whitelist](roles.md) a set request can introduce new roles.

In order to bypass the [capacity heuristic](#capacityHeuristic) check the
operator should set an optional `force` field:

        {
          "force": true,
          "role": "role1",
          ...
        }

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success (the set request was successful).
* `400 BadRequest`: Invalid arguments (e.g., quota for role exists, invalid
  JSON, non-scalar resources included).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
* `409 Conflict`: The capacity heuristic check failed due to insufficient
  resources.

<a name="removeRequest"></a>
## Remove

The operator can remove a previously set quota by sending an HTTP DELETE request
to the `/quota/<role>` endpoint. For example the following command removes
a previously set quota for `role1`:

    $ curl -X DELETE http://<master-ip>:<port>/quota/role1

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success (the remove request was successful).
* `400 BadRequest`: Invalid arguments (e.g., removing a quota for a role which
  does not have    any quota set).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.

<a name="statusRequest"></a>
## Status

The operator can query the configured quotas by sending a HTTP GET request
to the `/quota` endpoint.

    $ curl -X GET http://<master-ip>:<port>/quota

The response message body includes a JSON representation of the current quota
status, for example:

        {
          "infos": [
            {
              "role": "role1",
              "guarantee": [
                {
                  "name": "cpus",
                  "role": "*",
                  "type": "SCALAR",
                  "scalar": { "value": 12 }
                },
                {
                  "name": "mem",
                  "role": "*",
                  "type": "SCALAR",
                  "scalar": { "value": 6144 }
                }
              ]
            }
          ]
        }

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success.
* `401 Unauthorized`: Unauthenticated request.

# How does it work?

There are several stages in the lifetime of a quota issued by operator. First
the [quota set request is handled by the master](#requestProcessing), after that
the [allocator enforces the quota](#allocatorEnforcement).
Quotas can be [removed](#removeProcessing) by the operator.

It is important to understand that the enforcement of quota depends on
the allocator being used. A custom allocator could choose to handle quota in its
own way or even to ignore quota.
**The following section assumes the default
[wDRF](https://www.cs.berkeley.edu/~alig/papers/drf.pdf) allocator is used.**

Be aware that setting quota may affect other frameworks in the cluster,
because resources will be laid away and not offered to other frameworks.
Also note, that quota is only applicable for scalar resources (e.g., it is not
possible to set quota for port resources).

<a name="requestProcessing"></a>
## Quota Request Processing

When an operator submits a quota set request via the master `/quota` HTTP
endpoint, the following steps are triggered:

1. [Authenticate](authentication.md) the HTTP request.
2. Parse and validate the request.
   See [description of possible error codes](#setRequest).
3. [Authorize](authentication.md) the HTTP request if authorization is enabled.
4. Run the [capacity heuristic](#capacityHeuristic) if not disabled by
   [the `force` flag](#setRequest).
5. Reliably store quota. See [details on failover recovery](#failover).
6. [Rescind outstanding offers](#rescindOffers).

<a name="removeProcessing"></a>
The quota remove request processing is simpler and triggers the following steps:

1. [Authenticate](authentication.md) the HTTP request.
2. Validate the request.
   See [description of potential error codes](#removeRequest).
3. [Authorize](authentication.md) the HTTP request if authorization is enabled.
4. Reliably remove quota.

<a name="capacityHeuristic"></a>
### Capacity Heuristic Check

Misconfigured quota can render a cluster into a state where no offers are made
to any frameworks. For example imagine an operator setting quota 1000 CPUs
for a role `prosuction` (note the typo) in a cluster with a total capacity of
100 CPUs. In that case after the quota is accepted by the master, no offers will
 be made to any framework in any actual role (including `production`).

In order to prevent such extreme situations, the Mesos Master employs a capacity
heuristic check that ensures a quota set request can reasonably be satisfied
given the total cluster capacity. This heuristic tests whether the total quota,
including the new request, does not exceed the sum of total
non-statically-reserved cluster resources, i.e. the following inequality holds:

    total resources - statically reserved >= total quota + quota request

Please be advised that even if there are enough resources at the moment of
this check, agents may terminate at any time, rendering the cluster incapable
of satisfying the configured quotas.

A [`force` flag](#setRequest) can be set to bypass this check. For example, this
flag can be useful when the operator would like to configure a quota that
exceeds the current cluster capacity, but they know that additional cluster
resources will be added shortly.

<a name="rescindOffers"></a>
### Rescinding Outstanding Offers

When setting a new quota, the master rescinds outstanding offers. This avoids
situations where the quota request cannot be satisfied by the remaining
unoffered resources, but there are enough resources tied up in outstanding
offers to frameworks that have not accepted them yet. Hence, we rescind
outstanding offers with the following rules:

* Rescind at least as many resources as there are in the quota request.
* If at least one offer is to be rescinded from an agent, all offers from this
  agent are rescinded. This is done in order to make the potential offer bigger,
  which increases the chances that a quota'ed framework will be able to use the
  offer.
* Rescind offers from at least as many agents as there are frameworks in the
  role for which quota is being set. This enables (but does not guarantee, due
  to fair sharing) each framework in the role to receive an offer.

<a name="allocatorEnforcement"></a>
## Enforcement by wDRF Allocator

The wDRF allocator first allocates (or lays away if offers are declined)
resources to framework in roles with quota set. Once all quotas are
satisfied, it proceeds with the standard wDRF for all frameworks.

If there are multiple frameworks in a role with quota set, the standard wDRF
algorithm determines framework priority inside this role.

The default wDRF allocator considers only unreserved and non-revocable resources
as applicable towards quota.

<a name="failover"></a>
## Failover

If there is at least one role with quota set, the master failover recovery
changes significantly. The reason for this is that during the recovery there is
a period of time when not all agents have registered with the Master. Therefore
it is impossible to reason about whether all quotas are satisfied.

To address this issue, if upon recovery any previously set quota are detected,
the allocator enters recovery mode, during which the allocator
_does not issue offers_. The recovery mode---and therefore offer
suspension---ends when either:

* A certain amount of agents reregister (by default 80% of agents known before
  the failover), or
* a timeout expires (by default 10 minutes).

# Current Limitations

* The quota set request does not allow specifying the granularity of the
  requested resources (e.g. 10 CPUs on a single node).
* The quota set request does not allow to specify constraints (e.g. 2*5 cpus on
  disjoint nodes for an HA like setup).
* Quota is not allowed for the default role ‘*’ (see MESOS-3938).
* Currently it is not possible to update previously configured quotas. See
  [quota set request](#setRequest) for details.
