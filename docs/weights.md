---
title: Apache Mesos - Weights
layout: documentation
---

# Weights

In Mesos, __weights__ can be used to control the relative share of cluster
resources that is offered to different [roles](roles.md).

In Mesos 0.28 and earlier, weights can only be configured by specifying
the `--weights` command-line flag when starting the Mesos master. If a
role does not have a weight specified in the `--weights` flag, then the default
value (1.0) will be used. Weights cannot be changed without updating the flag
and restarting all Mesos masters.

Mesos 1.0 contains a [/weights](endpoints/master/weights.md) operator endpoint
that allows weights to be changed at runtime. The `--weights` command-line flag
is deprecated.

# Operator HTTP Endpoint

The master `/weights` HTTP endpoint enables operators to configure weights. The
endpoint currently offers a REST-like interface and supports the following operations:

* [Updating](#putRequest) weights with PUT.
* [Querying](#getRequest) the currently set weights with GET.

The endpoint can optionally use authentication and authorization. See the
[authentication guide](authentication.md) for details.

<a name="putRequest"></a>
## Update

The operator can update the weights by sending an HTTP PUT request to the `/weights`
endpoint.

An example request to the `/weights` endpoint could look like this (using the
JSON file below):

    $ curl -d @weights.json -X PUT http://<master-ip>:<port>/weights

For example, to set a weight of `2.0` for `role1` and set a weight of `3.5`
for `role2`, the operator can use the following `weights.json`:

        [
          {
            "role": "role1",
            "weight": 2.0
          },
          {
            "role": "role2",
            "weight": 3.5
          }
        ]

If the master is configured with an explicit [role whitelist](roles.md), the
request is only valid if all specified roles exist in the role whitelist.

Weights are now persisted in the registry on cluster bootstrap and after any
updates.  Once the weights are persisted in the registry, any Mesos master that
subsequently starts with `--weights` still specified will emit a warning and use
the registry value instead.

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success (the update request was successful).
* `400 BadRequest`: Invalid arguments (e.g., invalid JSON, non-positive weights).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.

<a name="getRequest"></a>
## Query

The operator can query the configured weights by sending an HTTP GET request
to the `/weights` endpoint.

    $ curl -X GET http://<master-ip>:<port>/weights

The response message body includes a JSON representation of the current
configured weights, for example:

        [
          {
            "role": "role2",
            "weight": 3.5
          },
          {
            "role": "role1",
            "weight": 2.0
          }
        ]

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success.
* `401 Unauthorized`: Unauthenticated request.
