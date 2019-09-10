---
title: Apache Mesos - Quota
layout: documentation
---

# Quota

When multiple users are sharing a cluster, the operator may want to set limits
on how many resources each user can use. Quota addresses this need and allows
operators to set these limits on a per-role basis.

* [Supported Resources](#supported-resources)
* [Setting Quotas](#updating-quotas)
* [Viewing Quotas](#viewing-quotas)
* [Deprecated: Quota Guarantees](#deprecated-quota-guarantees)
* [Implementation Notes](#implementation-notes)


## Supported Resources

The following resources have quota support:

* `cpus`
* `mem`
* `disk`
* `gpus`
* any custom resource of type `SCALAR`

The following resources do not have quota support:

* `ports`
* any custom resource of type `RANGES` or `SET`


## Updating Quotas

By default, every role has no resource limits. To modify the resource limits
for one or more roles, the v1 API `UPDATE_QUOTA` call is used. Note that this
call applies the update in an all-or-nothing manner, so that if one of the
role's quota updates is invalid or unauthorized, the entire request will not
go through.

Example:

```
curl --request POST \
     --url http://<master-ip>:<master-port>/api/v1/ \
     --header 'Content-Type: application/json' \
     --data '{
               "type": "UPDATE_QUOTA",
               "update_quota": {
                 "force": false,
                 "quota_configs": [
                   {
                     "role": "dev",
                     "limits": {
                       "cpus": { "value": 10 },
                       "mem":  { "value": 2048 },
                       "disk": { "value": 4096 }
                     }
                   },
                   {
                     "role": "test",
                     "limits": {
                       "cpus": { "value": 1 },
                       "mem":  { "value": 256 },
                       "disk": { "value": 512 }
                     }
                   }
                 ]
               }
             }'
```

* Note that the request will be denied if the current quota consumption is above
the provided limit. This check can be overriden by setting `force` to `true`.
* Note that the master will attempt to rescind a sufficient number of offers to
ensure that the role cannot exceed its limits.


## Viewing Quotas

### Web UI

The 'Roles' tab in the web ui displays resource accounting information for all
known roles. This includes the configured quota and the quota consumption.

### API

There are several endpoints for viewing quota related information.

The v1 API `GET_QUOTA` call will return the quota configuration:

```
$ curl --request POST \
     --url http://<master-ip>:<master-port>/api/v1/ \
     --header 'Content-Type: application/json' \
     --header 'Accept: application/json' \
     --data '{ "type": "GET_QUOTA" }'
```

Response:

```
{
  "type": "GET_QUOTA",
  "get_quota": {
    "status": {
      "infos": [
        {
          "configs" : [
            {
              "role": "dev",
              "limits": {
                "cpus": { "value": 10.0 },
                "mem":  { "value": 2048.0 },
                "disk": { "value": 4096.0 }
              }
            },
            {
              "role": "test",
              "limits": {
                "cpus": { "value": 1.0 },
                "mem":  { "value": 256.0 },
                "disk": { "value": 512.0 }
              }
            }
          ]
        }
      ]
    }
  }
}
```

To also view the quota consumption, use the `/roles` endpoint:

```
$ curl http://<master-ip>:<master-port>/roles
```

Response

```
{
  "roles": [
    {
      "name": "dev",
      "weight": 1.0,
      "quota":
      {
        "role": "dev",
        "limit": {
          "cpus": 10.0,
          "mem":  2048.0,
          "disk": 4096.0
        },
        "consumed": {
          "cpus": 2.0,
          "mem":  1024.0,
          "disk": 2048.0
        }
      },
      "allocated": {
        "cpus": 2.0,
        "mem":  1024.0,
        "disk": 2048.0
      },
      "offered": {},
      "reserved": {
        "cpus": 2.0,
        "mem":  1024.0,
        "disk": 2048.0
      },
      "frameworks": []
    }
  ]
}
```

### Quota Consumption

A role's quota consumption consists of its allocations and reservations.
In other words, even if reservations are not allocated, they are included
in the quota consumption. Offered resources are not charged against quota.


### Metrics

The following metric keys are exposed for quota:

* `allocator/mesos/quota/roles/<role>/resources/<resource>/guarantee`
* `allocator/mesos/quota/roles/<role>/resources/<resource>/limit`
* A quota consumption metric will be added via
  [MESOS-9123](https://issues.apache.org/jira/browse/MESOS-9123).


## Deprecated: Quota Guarantees

Prior to Mesos 1.9, the quota related APIs only exposed quota "guarantees"
which ensured a minimum amount of resources would be available to a role.
Setting guarantees also set implicit quota limits. In Mesos 1.9+, quota
limits are now exposed directly per the above documentation.

Quota guarantees are now deprecated in favor of using only quota limits.
Enforcement of quota guarantees required that Mesos holds back enough
resources to meet all of the unsatisfied quota guarantees. Since Mesos is
moving towards an optimistic offer model (to improve multi-role / multi-
scheduler scalability, see
[MESOS-1607](https://issues.apache.org/jira/browse/MESOS-1607)), it will
become no longer possible to enforce quota guarantees by holding back
resources. In such a model, quota limits are simple to enforce, but quota
guarantees would require a complex "effective limit" propagation model to
leave space for unsatisfied guarantees.

For these reasons, quota guarantees, while still functional in Mesos 1.9,
are now deprecated. A combination of limits and priority based preemption
will be simpler in an optimistic offer model.

For documentation on quota guarantees, please see the previous
documentation: [https://github.com/apache/mesos/blob/1.8.0/docs/quota.md](https://github.com/apache/mesos/blob/1.8.0/docs/quota.md)


## Implementation Notes

* Quota is not supported on nested roles (e.g. `eng/prod`).
* During failover, in order to correctly enforce limits, the allocator
  will be paused and will not issue offers until at least 80% agents
  re-register or 10 minutes elapses. These parameters will be made
  configurable: [MESOS-4073](https://issues.apache.org/jira/browse/MESOS-4073)
* Quota is SUPPORTED for the default `*` role now [MESOS-3938](https://issues.apache.org/jira/browse/MESOS-3938).
