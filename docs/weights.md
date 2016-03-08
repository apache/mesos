---
layout: documentation
---

# Weights

In Mesos, __weights__ are used to indicate forms of priority
among [roles](roles.md).

In Mesos 0.28 and earlier, weights can only be configured by specifying
the `--weights` command-line flag when starting the Mesos master. If a
role does not have a weight specified in the `--weights` flag, then the default
value (1.0) will be used. Weights cannot be changed without updating the flag
and restarting all Mesos masters.

In Mesos 0.29, a new endpoint `/weights` is introduced so an operator can
update weights at runtime. The `--weights` command-line flag is deprecated.

# Operator HTTP Endpoint

The master `/weights` HTTP endpoint enables operators to update weights. The
endpoint currently offers a REST-like interface and only supports an update
request with PUT.

The endpoint can optionally use authentication and authorization. See the
[authentication guide](authentication.md) for details.

An example request to the `/weights` endpoint could look like this (using the
JSON definitions below):

    $ curl -d jsonMessageBody -X PUT http://<master-ip>:<port>/weights

For example, to set a weight of `2.0` for `role1` and set a weight of `3.5`
for `role2`, the operator can use the following `jsonMessageBody`:

        [
          {
            "weight": 2.0,
            "role": "role1"
          },
          {
            "weight": 3.5,
            "role": "role2"
          }
        ]

An update request is always valid when master is configured with implicit
roles. However if the master is configured with an explicit [role whitelist](roles.md)
the request is only valid when all specified roles exist in the `whitelist`.

Weights are now persisted in the registry on cluster bootstrap and after any updates.
Once the weights are persisted in the registry, any Mesos master that subsequently starts
with `--weights` still specified will warn and use the registry value instead.

The operator will receive one of the following HTTP response codes:

* `200 OK`: Success (the update request was successful).
* `400 BadRequest`: Invalid arguments (e.g. invalid JSON, non-positive weights).
* `401 Unauthorized`: Unauthenticated request.
* `403 Forbidden`: Unauthorized request.
