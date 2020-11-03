---
title: Apache Mesos - Scheduler HTTP API
layout: documentation
---

# Scheduler HTTP API

A Mesos scheduler can be built in two different ways:

1. By using the `SchedulerDriver` C++ interface. The `SchedulerDriver` handles
the details of communicating with the Mesos master. Scheduler developers
implement custom scheduling logic by registering callbacks with the
`SchedulerDriver` for significant events, such as receiving a new resource offer
or a status update on a task. Because the `SchedulerDriver` interface is written
in C++, this typically requires that scheduler developers either use C++ or use
a C++ binding to their language of choice (e.g., JNI when using JVM-based
languages).

2. By using the new HTTP API. This allows Mesos schedulers to be developed
without using C++ or a native client library; instead, a custom scheduler
interacts with the Mesos master via HTTP requests, as described below. Although
it is theoretically possible to use the HTTP scheduler API "directly" (e.g., by
using a generic HTTP library), most scheduler developers should use a library for
their language of choice that manages the details of the HTTP API; see the
document on [HTTP API client libraries](api-client-libraries.md) for a list.

The v1 Scheduler HTTP API was introduced in Mesos 0.24.0. As of Mesos 1.0, it is
considered stable and is the recommended way to develop new Mesos schedulers.


## Overview

The scheduler interacts with Mesos via the [/api/v1/scheduler](endpoints/master/api/v1/scheduler.md) master endpoint. We refer to this endpoint with its suffix "/scheduler" in the rest of this document. This endpoint accepts HTTP POST requests with data encoded as JSON (Content-Type: application/json) or binary Protobuf (Content-Type: application/x-protobuf). The first request that a scheduler sends to "/scheduler" endpoint is called SUBSCRIBE and results in a streaming response ("200 OK" status code with Transfer-Encoding: chunked).

**Schedulers are expected to keep the subscription connection open as long as possible (barring errors in network, software, hardware, etc.) and incrementally process the response.** HTTP client libraries that can only parse the response after the connection is closed cannot be used. For the encoding used, please refer to **Events** section below.

All subsequent (non-`SUBSCRIBE`) requests to the "/scheduler" endpoint (see details below in **Calls** section) must be sent using a different connection than the one used for subscription. Schedulers can submit requests using more than one different HTTP connection.

The master responds to HTTP POST requests that require asynchronous processing with status **202 Accepted** (or, for unsuccessful requests, with 4xx or 5xx status codes; details in later sections). The **202 Accepted** response means that a request has been accepted for processing, not that the processing of the request has been completed. The request might or might not be acted upon by Mesos (e.g., master fails during the processing of the request). Any asynchronous responses from these requests will be streamed on the long-lived subscription connection.

The master responds to HTTP POST requests that can be answered synchronously and immediately with status **200 OK** (or, for unsuccessful requests, with 4xx or 5xx status codes; details in later sections), possibly including a response body encoded in JSON or Protobuf. The encoding depends on the **Accept** header present in the request (the default encoding is JSON).

## Calls

The following calls are currently accepted by the master. The canonical source of this information is [scheduler.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/scheduler/scheduler.proto). When sending JSON-encoded Calls, schedulers should encode raw bytes in Base64 and strings in UTF-8. All non-`SUBSCRIBE` calls should include the `Mesos-Stream-Id` header, explained in the [`SUBSCRIBE`](#subscribe) section. `SUBSCRIBE` calls should never include the `Mesos-Stream-Id` header.


<a id="recordio-response-format"></a>
### RecordIO response format

The response returned from the `SUBSCRIBE` call (see [below](#subscribe)) is encoded in RecordIO format, which essentially prepends to a single record (either JSON or serialized Protobuf) its length in bytes, followed by a newline and then the data. See [RecordIO Format](recordio.md) for details.

<a id="subscribe"></a>
### SUBSCRIBE

This is the first step in the communication process between the scheduler and the master. This is also to be considered as subscription to the "/scheduler" event stream.

To subscribe with the master, the scheduler sends an HTTP POST with a `SUBSCRIBE` message including the required FrameworkInfo, the list of initially suppressed roles and the initial offer constraints. The initially suppressed roles, as well as roles for which offer constraints are specified, must be contained in the set of roles in FrameworkInfo. Note that Mesos 1.11.0 simply ignores constraints for invalid roles, but this might change in the future.

Note that if "subscribe.framework_info.id" and "FrameworkID" are not set, the master considers the scheduler as a new one and subscribes it by assigning it a FrameworkID. The HTTP response is a stream in RecordIO format; the event stream begins with either a `SUBSCRIBED` event or an `ERROR` event (see details in **Events** section). The response also includes the `Mesos-Stream-Id` header, which is used by the master to uniquely identify the subscribed scheduler instance. This stream ID header should be included in all subsequent non-`SUBSCRIBE` calls sent over this subscription connection to the master. The value of `Mesos-Stream-Id` is guaranteed to be at most 128 bytes in length.

```
SUBSCRIBE Request (JSON):

POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Accept: application/json
Connection: close

{
   "type"		: "SUBSCRIBE",
   "subscribe"	: {
      "framework_info"	: {
        "user" :  "foo",
        "name" :  "Example HTTP Framework",
        "roles": ["test1", "test2"],
        "capabilities" : [{"type": "MULTI_ROLE"}]
      },
      "suppressed_roles" : ["test2"],
      "offer_constraints" : {
        "role_constraints": {
          "test1": {
            "groups": [{
              "attribute_constraints": [{
                "selector": {"attribute_name": "foo"},
                "predicate": {"exists": {}}
              }]
            }]
          }
        }
      }
   }
}

SUBSCRIBE Response Event (JSON):
HTTP/1.1 200 OK

Content-Type: application/json
Transfer-Encoding: chunked
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

<event length>
{
 "type"			: "SUBSCRIBED",
 "subscribed"	: {
     "framework_id"               : {"value":"12220-3440-12532-2345"},
     "heartbeat_interval_seconds" : 15
  }
}
<more events>
```

Alternatively, if "subscribe.framework_info.id" and "FrameworkID" are set, the master considers this a request from an already subscribed scheduler reconnecting after a disconnection (e.g., due to master/scheduler failover or network disconnection) and responds
with a `SUBSCRIBED` event. For further details, see the **Disconnections** section below.

NOTE: In the old version of the API, (re-)registered callbacks also included MasterInfo, which contained information about the master the driver currently connected to. With the new API, since schedulers explicitly subscribe with the leading master (see details below in **Master Detection** section), it's not relevant anymore.

NOTE: By providing a different FrameworkInfo and/or set of suppressed roles
and/or offer constraints, a re-subscribing scheduler can change some of the
fields of FrameworkInfo, the set of suppressed roles and/or offer constraints.
Allowed changes and their effects are consistent with those that can be
performed via `UPDATE_FRAMEWORK` call (see below).

If subscription fails for whatever reason (e.g., invalid request), an HTTP 4xx response is returned with the error message as part of the body and the connection is closed.

A scheduler can make additional HTTP requests to the "/scheduler" endpoint only after it has opened a persistent connection to it by sending a `SUBSCRIBE` request and received a `SUBSCRIBED` response. Calls made without subscription will result in "403 Forbidden" instead of a "202 Accepted" response. A scheduler might also receive a "400 Bad Request" response if the HTTP request is malformed (e.g., malformed HTTP headers).

Note that the `Mesos-Stream-Id` header should **never** be included with a `SUBSCRIBE` call; the master will always provide a new unique stream ID for each subscription.

### TEARDOWN
Sent by the scheduler when it wants to tear itself down. When Mesos receives this request it will shut down all executors (and consequently kill tasks). It then removes the framework and closes all open connections from this scheduler to the Master.

```
TEARDOWN Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "TEARDOWN"
}

TEARDOWN Response:
HTTP/1.1 202 Accepted
```

### ACCEPT
Sent by the scheduler when it accepts offer(s) sent by the master. The `ACCEPT` request includes the type of operations (e.g., launch task, launch task group, reserve resources, create volumes) that the scheduler wants to perform on the offers. Note that until the scheduler replies (accepts or declines) to an offer, the offer's resources are considered allocated to the offer's role and to the framework. Also, any of the offer's resources not used in the `ACCEPT` call (e.g., to launch a task or task group) are considered declined and might be reoffered to other frameworks, meaning that they will not be reoffered to the scheduler for the amount of time defined by the filter. The same `OfferID` cannot be used in more than one `ACCEPT` call. These semantics might change when we add new features to Mesos (e.g., persistence, reservations, optimistic offers, resizeTask, etc.).

The scheduler API uses `Filters.refuse_seconds` to specify the duration for which resources are considered declined. If `filters` is not set, then the default value defined in [mesos.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/mesos.proto) will be used.

NOTE: Mesos will cap `Filters.refuse_seconds` at 31536000 seconds (365 days).

The master will send task status updates in response to `LAUNCH` and `LAUNCH_GROUP` operations. For other types of operations, if an operation ID is specified, the master will send operation status updates in response.

For more information on running workloads using this call, see the [introduction to the `LAUNCH_GROUP` and `LAUNCH` operations](running-workloads.md).

```
ACCEPT Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id": {"value": "12220-3440-12532-2345"},
  "type": "ACCEPT",
  "accept": {
    "offer_ids": [
      {"value": "12220-3440-12532-O12"}
    ],
    "operations": [
      {
        "type": "LAUNCH",
        "launch": {
          "task_infos": [
            {
              "name": "My Task",
              "task_id": {"value": "12220-3440-12532-my-task"},
              "agent_id": {"value": "12220-3440-12532-S1233"},
              "executor": {
                "command": {
                  "shell": true,
                  "value": "sleep 1000"
                },
                "executor_id": {"value": "12214-23523-my-executor"}
              },
              "resources": [
                {
                  "allocation_info": {"role": "engineering"},
                  "name": "cpus",
                  "role": "*",
                  "type": "SCALAR",
                  "scalar": {"value": 1.0}
				        }, {
                  "allocation_info": {"role": "engineering"},
                  "name": "mem",
                  "role": "*",
                  "type": "SCALAR",
                  "scalar": {"value": 128.0}
				        }
              ],
              "limits": {
                "cpus": "Infinity",
                "mem": 512.0
              }
            }
          ]
        }
      }
    ],
    "filters": {"refuse_seconds": 5.0}
  }
}

ACCEPT Response:
HTTP/1.1 202 Accepted

```

### DECLINE
Sent by the scheduler to explicitly decline offer(s) received. Note that this is same as sending an `ACCEPT` call with no operations.

```
DECLINE Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "DECLINE",
  "decline"			: {
    "offer_ids"	: [
                   {"value" : "12220-3440-12532-O12"},
                   {"value" : "12220-3440-12532-O13"}
                  ],
    "filters"	: {"refuse_seconds" : 5.0}
  }
}

DECLINE Response:
HTTP/1.1 202 Accepted

```

### REVIVE
Sent by the scheduler to perform two actions:

1. Place the scheduler's role(s) in a non-`SUPPRESS`ed state in order to once again receive offers. No-op if the role is not suppressed.
2. Clears all filters for its role(s) that were previously set via `ACCEPT` and `DECLINE`.

If no role is specified, the operation will apply to all of the scheduler's subscribed roles.

```
REVIVE Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id" : {"value" : "12220-3440-12532-2345"},
  "type"         : "REVIVE",
  "revive"       : {"role": <one-of-the-subscribed-roles>}
}

REVIVE Response:
HTTP/1.1 202 Accepted

```

### KILL
Sent by the scheduler to kill a specific task. If the scheduler has a custom executor, the kill is forwarded to the executor; it is up to the executor to kill the task and send a `TASK_KILLED` (or `TASK_FAILED`) update. If the task hasn't yet been delivered to the executor when Mesos master or agent receives the kill request, a `TASK_KILLED` is generated and the task launch is not forwarded to the executor. Note that if the task belongs to a task group, killing of one task results in all tasks in the task group being killed. Mesos releases the resources for a task once it receives a terminal update for the task. If the task is unknown to the master, a `TASK_LOST` will be generated.

```
KILL Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "KILL",
  "kill"			: {
    "task_id"	:  {"value" : "12220-3440-12532-my-task"},
    "agent_id"	:  {"value" : "12220-3440-12532-S1233"}
  }
}

KILL Response:
HTTP/1.1 202 Accepted

```

### SHUTDOWN
Sent by the scheduler to shutdown a specific custom executor (NOTE: This is a new call that was not present in the old API). When an executor gets a shutdown event, it is expected to kill all its tasks (and send `TASK_KILLED` updates) and terminate. If an executor doesn't terminate within a certain timeout (configurable via the `--executor_shutdown_grace_period` agent flag), the agent will forcefully destroy the container (executor and its tasks) and transition its active tasks to `TASK_LOST`.

```
SHUTDOWN Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "SHUTDOWN",
  "shutdown"		: {
    "executor_id"	:  {"value" : "123450-2340-1232-my-executor"},
    "agent_id"		:  {"value" : "12220-3440-12532-S1233"}
  }
}

SHUTDOWN Response:
HTTP/1.1 202 Accepted

```

### ACKNOWLEDGE
Sent by the scheduler to acknowledge a status update. Note that with the new API, schedulers are responsible for explicitly acknowledging the receipt of status updates that have `status.uuid` set. These status updates are retried until they are acknowledged by the scheduler. The scheduler must not acknowledge status updates that do not have `status.uuid` set, as they are not retried. The `uuid` field contains raw bytes encoded in Base64.

```
ACKNOWLEDGE Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "ACKNOWLEDGE",
  "acknowledge"		: {
    "agent_id"	:  {"value" : "12220-3440-12532-S1233"},
    "task_id"	:  {"value" : "12220-3440-12532-my-task"},
    "uuid"		:  "jhadf73jhakdlfha723adf"
  }
}

ACKNOWLEDGE Response:
HTTP/1.1 202 Accepted

```

### ACKNOWLEDGE_OPERATION_STATUS
Sent by the scheduler to acknowledge an operation status update. Schedulers are responsible for explicitly acknowledging the receipt of status updates that have `status.uuid` set. These status updates are retried until they are acknowledged by the scheduler. The scheduler must not acknowledge status updates that do not have `status.uuid` set, as they are not retried. The `uuid` field contains raw bytes encoded in Base64.

```
ACKNOWLEDGE_OPERATION_STATUS Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id": { "value": "12220-3440-12532-2345" },
  "type": "ACKNOWLEDGE_OPERATION_STATUS",
  "acknowledge_operation_status": {
    "agent_id": { "value": "12220-3440-12532-S1233" },
    "resource_provider_id": { "value": "12220-3440-12532-rp" },
    "uuid": "jhadf73jhakdlfha723adf",
    "operation_id": "73jhakdlfha723adf"
  }
}

ACKNOWLEDGE_OPERATION_STATUS Response:
HTTP/1.1 202 Accepted

```

### RECONCILE
Sent by the scheduler to query the status of non-terminal tasks. This causes the master to send back `UPDATE` events for each task in the list. Tasks that are no longer known to Mesos will result in `TASK_LOST` updates. If the list of tasks is empty, master will send `UPDATE` events for all currently known tasks of the framework.

```
RECONCILE Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "RECONCILE",
  "reconcile"		: {
    "tasks"		: [
                   { "task_id"  : {"value" : "312325"},
                     "agent_id" : {"value" : "123535"}
                   }
                  ]
  }
}

RECONCILE Response:
HTTP/1.1 202 Accepted

```

### RECONCILE_OPERATIONS
Sent by the scheduler to query the status of non-terminal and terminal-but-unacknowledged operations. This causes the master to send back `UPDATE_OPERATION_STATUS` events for each operation in the list. If the list of operations is empty, the master will send events for all currently known operations of the framework.

```
RECONCILE_OPERATIONS Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Accept: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id": { "value": "12220-3440-12532-2345" },
  "type": "RECONCILE_OPERATIONS",
  "reconcile_operations": {
    "operations": [
      {
        "operation_id": { "value": "312325" },
        "agent_id": { "value": "123535" },
        "resource_provider_id": { "value": "927695" }
      }
    ]
  }
}

RECONCILE_OPERATIONS Response:
HTTP/1.1 202 Accepted

```

### MESSAGE
Sent by the scheduler to send arbitrary binary data to the executor. Mesos neither interprets this data nor makes any guarantees about the delivery of this message to the executor. `data` is raw bytes encoded in Base64.

```
MESSAGE Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "MESSAGE",
  "message"			: {
    "agent_id"       : {"value" : "12220-3440-12532-S1233"},
    "executor_id"    : {"value" : "my-framework-executor"},
    "data"           : "adaf838jahd748jnaldf"
  }
}

MESSAGE Response:
HTTP/1.1 202 Accepted

```

### REQUEST
Sent by the scheduler to request resources from the master/allocator. The built-in hierarchical allocator simply ignores this request but other allocators (modules) can interpret this in a customizable fashion.

```
Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "REQUEST",
  "requests"		: [
      {
         "agent_id"       : {"value" : "12220-3440-12532-S1233"},
         "resources"      : {}
      }
  ]
}

REQUEST Response:
HTTP/1.1 202 Accepted

```

### SUPPRESS
Sent by the scheduler when it doesn't need offers for a given set of its roles. When Mesos master receives this request, it will stop sending offers for the given set of roles to the framework. As a special case, if roles are not specified, all subscribed roles of this framework are suppressed.

Note that master continues to send offers to other subscribed roles of this framework that are not suppressed. Also, status updates about tasks, executors and agents are not affected by this call. 

If the scheduler wishes to receive offers for the suppressed roles again (e.g., it needs to schedule new workloads), it can send `REVIVE` call.

```
SUPPRESS Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Mesos-Stream-Id: 130ae4e3-6b13-4ef4-baa9-9f2e85c3e9af

{
  "framework_id" : {"value" : "12220-3440-12532-2345"},
  "type"         : "SUPPRESS",
  "suppress"     : {"roles": <an-array-of-strings>}
}

SUPPRESS Response:
HTTP/1.1 202 Accepted

```

### UPDATE_FRAMEWORK

Sent by the scheduler to change fields of its `FrameworkInfo` and/or the set of
suppressed roles and/or offer constraints. Allowed changes and their effects
are consistent with changing the same fields via re-subscribing.

#### Disallowed updates
Updating the following `FrameworkInfo` fields is not allowed:
  * `principal` (mainly because "changing a principal" effectively means
  a transfer of a framework by an original principal to the new one; secure
  mechanism for such transfer is yet to be developed)
  * `user`
  * `checkpoint`

`UPDATE_FRAMEWORK` call trying to update any of these fields is not valid,
unlike an attempt to change `user`/`checkpoint` when resubscribing, in which
case the new value is ignored.

#### Updating framework roles
Updating `framework_info.roles` and `suppressed_roles` is supported.
In a valid `UPDATE_FRAMEWORK` call, new suppressed roles must be a (potentially
empty) subset of new framework roles.

Updating roles has the following effects:
  * After the call is processed, master will be sending offers to all
  non-suppressed roles of the framework.
  * Offers to old framework roles removed by this call will be rescinded.
  * Offers to roles from suppressed set will NOT be rescinded.
  * For roles that were transitioned out of suppressed, offer filters (set by
  ACCEPT/DECLINE) will be cleared.
  will be cleared.
  * Other framework objects that use roles removed by this call (for example,
  tasks) are not affected.

#### Updating offer constraints
For the `UPDATE_FRAMEWORK` call to be successfull, the `offer_constraints`
field, if present, must be internally valid (for the constraints validity
criteria, please refer to comments in
[scheduler.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/scheduler/scheduler.proto))

As of 1.11.0, Mesos ignores offer constraints for roles other than valid roles
in `framework_info.roles`; future versions of Mesos are going to treat such
offer constraints as invalid.

Updated offer constraints have an immediate effect on offer generation after
update, but have no effect on already outstanding offers. Frameworks should not
expect that offers they receive right after the `UPDATE_FRAMEWORK` call
will satisfy the new constraints.

#### Updating other fields
  * Updating `name`, `hostname`, `webui_url` and `labels` is fully supported
  by Mesos; these updates are simply propagated to Mesos API endpoints.
  * Updating `failover_timeout` and `offer_filters` is supported. Note that
  there is no way to guarantee that offers issued when the old `offer_filters`
  were in place will not be received by the framework after the master applies
  the update.
  * Schedulers can add capabilities via updating `capabilities` field. The call
  attempting to remove a capability is not considered invalid; however, there
  is no guarantee that it is safe for the framework to remove the capability.
  If you really need your framewok to be able to remove a capability, please
  reach out to the Mesos dev/user list (dev@mesos.apache.org or
  user@mesos.apache.org).
  In future, to prevent accidental unsafe downgrade of frameworks, Mesos will
  need to implement minimum capabilities for schedulers (similarly to minimum
  master/agent capabilities, see
  [MESOS-8878](https://issues.apache.org/jira/browse/MESOS-8878)).


```

UPDATE_FRAMEWORK Request (JSON):

POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json
Accept: application/json
Connection: close

{
   "type"		: "UPDATE_FRAMEWORK",
   "update_framework"	: {
      "framework_info"	: {
        "user" :  "foo",
        "name" :  "Example HTTP Framework",
        "roles": ["test1", "test2"],
        "capabilities" : [{"type": "MULTI_ROLE"}]
      },
      "suppressed_roles" : ["test2"]
      "offer_constraints" : {
        "role_constraints": {
          "test1": {
            "groups": [{
              "attribute_constraints": [{
                "selector": {"attribute_name": "foo"},
                "predicate": {"exists": {}}
              }]
            }]
          }
        }
      }
  }
}

UPDATE_FRAMEWORK Response:
HTTP/1.1 200 OK
```

Response codes:
  * "200 OK" after the update has been successfully applied by the master and
  sent to the agents.
  * "400 Bad request" if the call was not valid or authorizing the call failed.
  * "403 Forbidden" if the principal was declined authorization to use the
  provided FrameworkInfo. (Typical authorizer implementations will check
  authorization to use specified roles.)

No partial updates occur in error cases: either all fields are updated or none
of them.

NOTE: In Mesos 1.9, effects of changing roles or suppressed roles set via
UPDATE_FRAMEWORK could be potentially reordered with related effects of
`ACCEPT`/`DECLINE`/`SUPPRESS`/`REVIVE` or another `UPDATE_FRAMEWORK`;
to avoid such reordering, it was necessary to wait for UPDATE_FRAMEWORK response
before issuing the next call. This issue has been fixed in Mesos 1.10.0 (see
[MESOS-10056](https://issues.apache.org/jira/browse/MESOS-10056)).

## Events

Schedulers are expected to keep a **persistent** connection to the "/scheduler" endpoint (even after getting a `SUBSCRIBED` HTTP Response event). This is indicated by the "Connection: keep-alive" and "Transfer-Encoding: chunked" headers with *no* "Content-Length" header set. All subsequent events that are relevant to this framework generated by Mesos are streamed on this connection. The master encodes each Event in RecordIO format, i.e., string representation of the length of the event in bytes followed by JSON or binary Protobuf (possibly compressed) encoded event. The length of an event is a 64-bit unsigned integer (encoded as a textual value) and will never be "0". Also, note that the RecordIO encoding should be decoded by the scheduler whereas the underlying HTTP chunked encoding is typically invisible at the application (scheduler) layer. The type of content encoding used for the events will be determined by the accept header of the POST request (e.g., Accept: application/json).

The following events are currently sent by the master. The canonical source of this information is at [scheduler.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/scheduler/scheduler.proto). Note that when sending JSON encoded events, master encodes raw bytes in Base64 and strings in UTF-8.

### SUBSCRIBED
The first event sent by the master when the scheduler sends a `SUBSCRIBE` request, if authorization / validation succeeds. See `SUBSCRIBE` in Calls section for the format.


### OFFERS
Sent by the master whenever there are new resources that can be offered to the framework. Each offer corresponds to a set of resources on an agent and is allocated to one of roles the framework is subscribed to. Until the scheduler 'Accept's or 'Decline's an offer the resources are considered allocated to the scheduler, unless the offer is otherwise rescinded, e.g., due to a lost agent or `--offer_timeout`.

```
OFFERS Event (JSON)

<event-length>
{
  "type"	: "OFFERS",
  "offers"	: [
    {
      "allocation_info": { "role": "engineering" },
      "id"             : {"value": "12214-23523-O235235"},
      "framework_id"   : {"value": "12124-235325-32425"},
      "agent_id"       : {"value": "12325-23523-S23523"},
      "hostname"       : "agent.host",
      "resources"      : [
                          {
                           "allocation_info": { "role": "engineering" },
                           "name"   : "cpus",
                           "type"   : "SCALAR",
                           "scalar" : {"value" : 2},
                           "role"   : "*"
                          }
                         ],
      "attributes"     : [
                          {
                           "name"   : "os",
                           "type"   : "TEXT",
                           "text"   : {"value" : "ubuntu16.04"}
                          }
                         ],
      "executor_ids"   : [
                          {"value" : "12214-23523-my-executor"}
                         ]
    }
  ]
}
```

### RESCIND
Sent by the master when a particular offer is no longer valid (e.g., the agent corresponding to the offer has been removed) and hence needs to be rescinded. Any future calls (`ACCEPT` / `DECLINE`) made by the scheduler regarding this offer will be invalid.

```
RESCIND Event (JSON)

<event-length>
{
  "type"	: "RESCIND",
  "rescind"	: {
    "offer_id"	: { "value" : "12214-23523-O235235"}
  }
}
```

### UPDATE
Sent by the master whenever there is a status update that is generated by the executor, agent or master. Status updates should be used by executors to reliably communicate the status of the tasks that they manage. It is crucial that a terminal update (e.g., `TASK_FINISHED`, `TASK_KILLED`, `TASK_FAILED`) is sent by the executor as soon as the task terminates, in order for Mesos to release the resources allocated to the task. It is also the responsibility of the scheduler to explicitly acknowledge the receipt of status updates that are reliably retried. See `ACKNOWLEDGE` in the Calls section above for the semantics. Note that `uuid` and `data` are raw bytes encoded in Base64.


```
UPDATE Event (JSON)

<event-length>
{
  "type"	: "UPDATE",
  "update"	: {
    "status"	: {
        "task_id"	: { "value" : "12344-my-task"},
        "state"		: "TASK_RUNNING",
        "source"	: "SOURCE_EXECUTOR",
        "uuid"		: "adfadfadbhgvjayd23r2uahj",
        "bytes"		: "uhdjfhuagdj63d7hadkf"
      }
  }
}
```

### UPDATE_OPERATION_STATUS
Sent by the master whenever there is an update to the state of an operation for which the scheduler requested feedback by setting the operation's `id` field. It is the responsibility of the scheduler to explicitly acknowledge the receipt of any status updates which have their `uuid` field set, as this indicates that the update will be retried until acknowledgement is received. This ensures that such updates are delivered reliably. See `ACKNOWLEDGE_OPERATION_STATUS` in the Calls section above for the relevant acknowledgement semantics. Note that the `uuid` field contains raw bytes encoded in Base64.

```
UPDATE_OPERATION_STATUS Event (JSON)

<event-length>
{
  "type"	: "UPDATE_OPERATION_STATUS",
  "update_operation_status"	: {
    "status"	: {
        "operation_id" : { "value" : "operation-1234"},
        "state"        : "OPERATION_FAILED",
        "uuid"         : "adfadfadbhgvjayd23r2uahj",
        "agent_id"     : { "value" : "12214-23523-S235235"},
        "resource_provider_id" : { "value" : "83978-17885-1089645"}
      }
  }
}
```

### MESSAGE
A custom message generated by the executor that is forwarded to the scheduler by the master. This message is not interpreted by Mesos and is only forwarded (without reliability guarantees) to the scheduler. It is up to the executor to retry if the message is dropped for any reason. The `data` field contains raw bytes encoded as Base64.

```
MESSAGE Event (JSON)

<event-length>
{
  "type"	: "MESSAGE",
  "message"	: {
    "agent_id"		: { "value" : "12214-23523-S235235"},
    "executor_id"	: { "value" : "12214-23523-my-executor"},
    "data"			: "adfadf3t2wa3353dfadf"
  }
}
```


### FAILURE
Sent by the master when an agent is removed from the cluster (e.g., failed health checks) or when an executor is terminated. This event coincides with receipt of terminal `UPDATE` events for any active tasks belonging to the agent or executor and receipt of `RESCIND` events for any outstanding offers belonging to the agent. Note that there is no guaranteed order between the `FAILURE`, `UPDATE`, and `RESCIND` events.

```
FAILURE Event (JSON)

<event-length>
{
  "type"	: "FAILURE",
  "failure"	: {
    "agent_id"		: { "value" : "12214-23523-S235235"},
    "executor_id"	: { "value" : "12214-23523-my-executor"},
    "status"		: 1
  }
}
```

### ERROR
Can be sent either:

* As the first event (in lieu of `SUBSCRIBED`) when the scheduler's `SUBSCRIBE` request is invalid (e.g. invalid `FrameworkInfo`) or unauthorized (e.g., a framework is not authorized to subscribe with some of the given `FrameworkInfo.roles`).
* When an asynchronous error event is generated (e.g. the master detects a newer subscription from a failed over instance of the scheduler).

It is recommended that the framework abort when it receives an error and retry subscription as necessary.

```
ERROR Event (JSON)

<event-length>
{
  "type"	: "ERROR",
  "message"	: "Framework is not authorized"
}
```

### HEARTBEAT
This event is periodically sent by the master to inform the scheduler that a connection is alive. This also helps ensure that network intermediates do not close the persistent subscription connection due to lack of data flow. See the next section on how a scheduler can use this event to deal with network partitions.

```
HEARTBEAT Event (JSON)

<event-length>
{
  "type"	: "HEARTBEAT"
}
```

## Disconnections

Master considers a scheduler disconnected if the persistent subscription connection (opened via `SUBSCRIBE` request) to "/scheduler" breaks. The connection could break for several reasons, e.g., scheduler restart, scheduler failover, network error. Note that the master doesn't keep track of non-subscription connection(s) to
"/scheduler" because it is not expected to be a persistent connection.

If master realizes that the subscription connection is broken, it marks the scheduler as "disconnected" and starts a failover timeout (failover timeout is part of FrameworkInfo). It also drops any pending events in its queue. Additionally, it rejects subsequent non-subscribe HTTP requests to "/scheduler" with "403 Forbidden", until the scheduler subscribes again with "/scheduler". If the scheduler *does not* re-subscribe within the failover timeout, the master considers the scheduler gone forever and shuts down all its executors, thus killing all its tasks. Therefore, all production schedulers are recommended to use a high value (e.g., 4 weeks) for the failover timeout.

NOTE: To force shutdown of a framework before the failover timeout elapses (e.g., during framework development and testing), either the framework can send the `TEARDOWN` call (part of the Scheduler API) or an operator can use the [/teardown](endpoints/master/teardown.md) master endpoint (part of the Operator API).

If the scheduler realizes that its subscription connection to "/scheduler" is broken or the master has changed (e.g., via ZooKeeper), it should resubscribe (using a backoff strategy). This is done by sending a `SUBSCRIBE` request (with framework ID set) on a **new** persistent connection to the "/scheduler" endpoint on the (possibly new) master. It should not send new non-subscribe HTTP requests to "/scheduler" unless it receives a `SUBSCRIBED` event; such requests will result in "403 Forbidden".

If the master does not realize that the subscription connection is broken but the scheduler realizes it, the scheduler might open a new persistent connection to
"/scheduler" via `SUBSCRIBE`. In this case, the master closes the existing subscription connection and allows subscription on the new connection. The invariant here is that only one persistent subscription connection for a given framework ID is allowed on the master.

The master uses the `Mesos-Stream-Id` header to distinguish scheduler instances from one another. In the case of highly available schedulers with multiple instances, this can prevent unwanted behavior in certain failure scenarios. Each unique `Mesos-Stream-Id` is valid only for the life of a single subscription connection. Each response to a `SUBSCRIBE` request contains a `Mesos-Stream-Id`, and this ID must be included with all subsequent non-subscribe calls sent over that subscription connection. Whenever a new subscription connection is established, a new stream ID is generated and should be used for the life of that connection.

### Network partitions

In the case of a network partition, the subscription connection between the scheduler and master might not necessarily break. To be able to detect this scenario, master periodically (e.g., 15s) sends `HEARTBEAT` events (similar to Twitter's Streaming API). If a scheduler doesn't receive a bunch (e.g., 5) of these heartbeats within a time window, it should immediately disconnect and try to resubscribe. It is highly recommended for schedulers to use an exponential backoff strategy (e.g., up to a maximum of 15s) to avoid overwhelming the master while reconnecting. Schedulers can use a similar timeout (e.g., 75s) for receiving responses to any HTTP requests.

## Master detection

Mesos has a high-availability mode that uses multiple Mesos masters; one active master (called the leader or leading master) and several standbys in case it fails. The masters elect the leader, with ZooKeeper coordinating the election. For more details please refer to the [documentation](high-availability.md).

Schedulers are expected to make HTTP requests to the leading master. If requests are made to a non-leading master a "HTTP 307 Temporary Redirect" will be received with the "Location" header pointing to the leading master.

Example subscription workflow with redirection when the scheduler hits a non-leading master.

```
Scheduler -> Master
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost1:5050
Content-Type: application/json
Accept: application/json
Connection: keep-alive

{
  "framework_info"	: {
    "user" :  "foo",
    "name" :  "Example HTTP Framework"
  },
  "type"			: "SUBSCRIBE"
}

Master -> Scheduler
HTTP/1.1 307 Temporary Redirect
Location: masterhost2:5050


Scheduler -> Master
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost2:5050
Content-Type: application/json
Accept: application/json
Connection: keep-alive

{
  "framework_info"	: {
    "user" :  "foo",
    "name" :  "Example HTTP Framework"
  },
  "type"			: "SUBSCRIBE"
}
```

If the scheduler knows the list of master's hostnames for a cluster, it could use this mechanism to find the leading master to subscribe with. Alternatively, the scheduler could use a library that detects the leading master given a ZooKeeper (or etcd) URL. For a C++ library that does ZooKeeper based master detection please look at `src/scheduler/scheduler.cpp`.
