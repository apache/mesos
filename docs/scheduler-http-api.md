---
layout: documentation
---

# Scheduler HTTP API

Mesos 0.24.0 added **experimental** support for v1 Scheduler HTTP API.


## Overview

The scheduler interacts with Mesos via  "/api/v1/scheduler" endpoint hosted by the Mesos master. The fully qualified URL of the endpoint might look like:

	http://masterhost:5050/api/v1/scheduler

Note that we refer to this endpoint with its suffix "/scheduler" in the rest of this document. This endpoint accepts HTTP POST requests with data encoded as JSON (Content-Type: application/json) or binary Protobuf (Content-Type: application/x-protobuf). The first request that a scheduler sends to "/scheduler" endpoint is called SUBSCRIBE and results in a streaming response ("200 OK" status code with Transfer-Encoding: chunked). **Schedulers are expected to keep the subscription connection open as long as possible (barring errors in network, software, hardware etc.) and incrementally process the response** (NOTE: HTTP client libraries that can only parse the response after the connection is closed cannot be used). For the encoding used, please refer to **Events** section below.

All the subsequent (non subscribe) requests to "/scheduler" endpoint (see details below in **Calls** section) must be sent using a different connection(s) than the one being used for subscription. Master responds to these HTTP POST requests with "202 Accepted" status codes (or, for unsuccessful requests, with 4xx or 5xx status codes; details in later sections). The "202 Accepted" response means that a request has been accepted for processing, not that the processing of the request has been completed. The request might or might not be acted upon by Mesos (e.g., master fails during the processing of the request). Any asynchronous responses from these requests will be streamed on the long-lived subscription connection.


## Calls

The following calls are currently accepted by the master. The canonical source of this information is [scheduler.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/scheduler/scheduler.proto) (NOTE: The protobuf definitions are subject to change before the beta API is finalized). Note that when sending JSON encoded Calls, schedulers should encode raw bytes in Base64 and strings in UTF-8.


<a id="recordio-response-format"></a>
### RecordIO response format

The response returned from the `SUBSCRIBE` call (see [below](#subscribe)) is encoded in RecordIO format, which essentially prepends to a single record (either JSON or serialized Protobuf) its length in bytes, followed by a newline and then the data:

The [BNF grammar](http://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.1) for a RecordIO-encoded streaming response is:

```
    records         = *record

    record          = record-size LF record-data

    record-size     = 1*DIGIT
    record-data     = record-size(OCTET)
```

`record-size` should be interpreted as an unsigned 64-bit integer (`uint64`).

For example, a stream may look like:

```
128\n
{"type": "SUBSCRIBED","subscribed": {"framework_id": {"value":"12220-3440-12532-2345"},...}104\n
{"framework_id": {"value": "12220-3440-12532-2345"},...{"value" : "12220-3440-12532-O12"},}208\n
...
```

In pseudo-code, this could be parsed with something like the following:

```
  while (true) {
    do {
      lengthBytes = readline()
    } while (lengthBytes.length < 1)

    messageLength = parseInt(lengthBytes);
    messageBytes = read(messageLength);
    process(messageBytes);
  }
```

Network intermediaries e.g. proxies are free to change the chunk boundaries and this should not have any effect on the recipient application (scheduler layer). We wanted a way to delimit/encode two events for JSON/Protobuf responses consistently and RecordIO format allowed us to do that.


<a id="subscribe"></a>
### SUBSCRIBE

This is the first step in the communication process between the scheduler and the master. This is also to be considered as subscription to the "/scheduler" events stream.

To subscribe with the master, the scheduler sends a HTTP POST request with encoded  `SUBSCRIBE` message with the required FrameworkInfo. Note that if "subscribe.framework_info.id" is not set, master considers the scheduler as a new one and subscribes it by assigning it a FrameworkID. The HTTP response is a stream with RecordIO encoding, with the first event being `SUBSCRIBED` event (see details in **Events** section).

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
        "name" :  "Example HTTP Framework"
      },

      "force" : true
  }
}

SUBSCRIBE Response Event (JSON):
HTTP/1.1 200 OK

Content-Type: application/json
Transfer-Encoding: chunked

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

Alternatively, if "subscribe.framework_info.id" is set, master considers this a request from an already subscribed scheduler reconnecting after a disconnection (e.g., due to failover or network disconnection) and responds with `SUBSCRIBED` event containing the same FrameworkID. The "subscribe.force" field describes how the master reacts when multiple scheduler instances (with the same framework id) attempt to subscribe with the master at the same time (e.g., due to network partition). See the semantics in **Disconnections** section below.

NOTE: In the old version of the API, (re-)registered callbacks also included MasterInfo, which contained information about the master the driver currently connected to. With the new API, since schedulers explicitly subscribe with the leading master (see details below in **Master Detection** section), it's not relevant anymore.

If subscription fails for whatever reason (e.g., invalid request), a HTTP 4xx response is returned with the error message as part of the body and the connection is closed.

Scheduler must make additional HTTP requests to the "/scheduler" endpoint only after it has opened a persistent connection to it by sending a `SUBSCRIBE` request and received a `SUBSCRIBED` response. Calls made without subscription will result in a "403 Forbidden" instead of a "202 Accepted" response. A scheduler might also receive a "400 Bad Request" response if the HTTP request is malformed (e.g., malformed HTTP headers).

### TEARDOWN
Sent by the scheduler when it wants to tear itself down. When Mesos receives this request it will shut down all executors (and consequently kill tasks) and remove persistent volumes (if requested). It then removes the framework and closes all open connections from this scheduler to the Master.

```
TEARDOWN Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "TEARDOWN",
}

TEARDOWN Response:
HTTP/1.1 202 Accepted
```

### ACCEPT
Sent by the scheduler when it accepts offer(s) sent by the master. The `ACCEPT` request includes the type of operations (e.g., launch task, reserve resources, create volumes) that the scheduler wants to perform on the offers. Note that until the scheduler replies (accepts or declines) to an offer, its resources are considered allocated to the framework. Also, any of the offer's resources not used in the `ACCEPT` call (e.g., to launch a task) are considered declined and might be reoffered to other frameworks. In other words, the same `OfferID` cannot be used in more than one `ACCEPT` call. These semantics might change when we add new features to Mesos (e.g., persistence, reservations, optimistic offers, resizeTask, etc.).

```
ACCEPT Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "ACCEPT",
  "accept"			: {
    "offer_ids"		: [
                       {"value" : "12220-3440-12532-O12"},
                       {"value" : "12220-3440-12532-O12"}
                      ],
    "operations"	: [ {"type" : "LAUNCH", "launch" : {...}} ],
    "filters"		: {...}
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

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "DECLINE",
  "decline"			: {
    "offer_ids"	: [
                   {"value" : "12220-3440-12532-O12"},
                   {"value" : "12220-3440-12532-O13"}
                  ],
    "filters"	: {...}
  }
}

DECLINE Response:
HTTP/1.1 202 Accepted

```

### REVIVE
Sent by the scheduler to remove any/all filters that it has previously set via `ACCEPT` or `DECLINE` calls.

```
REVIVE Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "REVIVE",
}

REVIVE Response:
HTTP/1.1 202 Accepted

```

### KILL
Sent by the scheduler to kill a specific task. If the scheduler has a custom executor, the kill is forwarded to the executor and it is up to the executor to kill the task and send a `TASK_KILLED` (or `TASK_FAILED`) update. Mesos releases the resources for a task once it receives a terminal update for the task. If the task is unknown to the master, a `TASK_LOST` will be generated.

```
KILL Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

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
Sent by the scheduler to shutdown a specific custom executor (NOTE: This is a new call that was not present in the old API). When an executor gets a shutdown event, it is expected to kill all its tasks (and send `TASK_KILLED` updates) and terminate. If an executor doesn't terminate within a certain timeout (configurable via  "--executor_shutdown_grace_period" agent flag), the agent will forcefully destroy the container (executor and its tasks) and transitions its active tasks to `TASK_LOST`.

```
SHUTDOWN Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

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
Sent by the scheduler to acknowledge a status update. Note that with the new API, schedulers are responsible for explicitly acknowledging the receipt of status updates that have "status.uuid()" set. These status updates are reliably retried until they are acknowledged by the scheduler. The scheduler must not acknowledge status updates that do not have "status.uuid()" set as they are not retried. "uuid" is raw bytes encoded in Base64.

```
ACKNOWLEDGE Request (JSON):
POST /api/v1/scheduler  HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

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

### RECONCILE
Sent by the scheduler to query the status of non-terminal tasks. This causes the master to send back `UPDATE` events for each task in the list. Tasks that are no longer known to Mesos will result in `TASK_LOST` updates. If the list of tasks is empty, master will send `UPDATE` events for all currently known tasks of the framework.

```
RECONCILE Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "RECONCILE",
  "reconcile"		: {
    "tasks"		: [
                   { "task_id"  : { "value" : "312325" },
                     "agent_id" : { "value" : "123535" }
                   }
                  ]
  }
}

RECONCILE Response:
HTTP/1.1 202 Accepted

```

### MESSAGE
Sent by the scheduler to send arbitrary binary data to the executor. Note that Mesos neither interprets this data nor makes any guarantees about the delivery of this message to the executor. "data" is raw bytes encoded in Base64.

```
MESSAGE Request (JSON):
POST /api/v1/scheduler   HTTP/1.1

Host: masterhost:5050
Content-Type: application/json

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

{
  "framework_id"	: {"value" : "12220-3440-12532-2345"},
  "type"			: "REQUEST",
  "requests"		: [
      {
         "agent_id"       : {"value" : "12220-3440-12532-S1233"},
         "resources"      : {}
      },
  ]
}

REQUEST Response:
HTTP/1.1 202 Accepted

```

## Events

Scheduler is expected to keep a **persistent** connection open to "/scheduler" endpoint even after getting a SUBSCRIBED HTTP Response event. This is indicated by "Connection: keep-alive" and "Transfer-Encoding: chunked" headers with *no* "Content-Length" header set. All subsequent events that are relevant to this framework  generated by Mesos are streamed on this connection. Master encodes each Event in RecordIO format, i.e., string representation of length of the event in bytes followed by JSON or binary Protobuf  (possibly compressed) encoded event. Note that the value of length will never be '0' and the size of the length will be the size of unsigned integer (i.e., 64 bits). Also, note that the RecordIO encoding should be decoded by the scheduler whereas the underlying HTTP chunked encoding is typically invisible at the application (scheduler) layer. The type of content encoding used for the events will be determined by the accept header of the POST request (e.g., Accept: application/json).

The following events are currently sent by the master. The canonical source of this information is at [scheduler.proto](include/mesos/v1/scheduler/scheduler.proto). Note that when sending JSON encoded events, master encodes raw bytes in Base64 and strings in UTF-8.

### SUBSCRIBED
The first event sent by the master when the scheduler sends a `SUBSCRIBE` request on the persistent connection. See `SUBSCRIBE` in Calls section for the format.


### OFFERS
Sent by the master whenever there are new resources that can be offered to the framework. Each offer corresponds to a set of resources on an agent. Until the scheduler 'Accept's or 'Decline's an offer the resources are considered allocated to the scheduler, unless the offer is otherwise rescinded, e.g. due to a lost agent or `--offer_timeout`.

```
OFFERS Event (JSON)
<event-length>
{
  "type"	: "OFFERS",
  "offers"	: [
    {
      "offer_id":{"value": "12214-23523-O235235"},
      "framework_id":{"value": "12124-235325-32425"},
      "agent_id":{"value": "12325-23523-S23523"},
      "hostname":"agent.host",
      "resources":[...],
      "attributes":[...],
      "executor_ids":[]
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

### MESSAGE
A custom message generated by the executor that is forwarded to the scheduler by the master. Note that this message is not interpreted by Mesos and is only forwarded (without reliability guarantees) to the scheduler. It is up to the executor to retry if the message is dropped  for any reason. Note that `data` is raw bytes encoded as Base64.

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
Sent by the master when an agent is removed from the cluster (e.g., failed health checks) or when an executor is terminated. Note that, this event coincides with receipt of terminal `UPDATE` events for any active tasks belonging to the agent or executor and receipt of `RESCIND` events for any outstanding offers belonging to the agent. Note that there is no guaranteed order between the `FAILURE`, `UPDATE` and `RESCIND` events.

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
Sent by the master when an asynchronous error event is generated (e.g., a framework is not authorized to subscribe with the given role). It is recommended that the framework abort when it receives an error and retry subscription as necessary.

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
  "type"	: "HEARTBEAT",
}
```

## Disconnections

Master considers a scheduler disconnected if the persistent subscription connection (opened via `SUBSCRIBE` request) to "/scheduler" breaks. The connection could break for several reasons, e.g., scheduler restart, scheduler failover, network error. Note that the master doesn't keep track of non-subscription connection(s) to
"/scheduler" because it is not expected to be a persistent connection.

If master realizes that the subscription connection is broken, it marks the scheduler as "disconnected" and starts a failover timeout (failover timeout is part of FrameworkInfo). It also drops any pending events in its queue. Additionally, it rejects subsequent non-subscribe HTTP requests to "/scheduler" with "403 Forbidden", until the scheduler subscribes again with "/scheduler". If the scheduler *does not* re-subscribe within the failover timeout, the master considers the scheduler gone forever and shuts down all its executors, thus killing all its tasks. Therefore, all production schedulers are recommended to use a high value (e.g., 4 weeks) for the failover timeout.

NOTE: To force shutdown a framework before the framework timeout elapses (e.g., during framework development and testing), either the framework can send `TEARDOWN` call (part of Scheduler API) or an operator can use the "/master/teardown" endpoint (part of Operator API).

If the scheduler realizes that its subscription connection to "/scheduler" is broken, it should attempt to open a new persistent connection to the
"/scheduler" (on possibly new master based on the result of master detection) and resubscribe. It should not send new non-subscribe HTTP requests to "/scheduler" unless it gets a `SUBSCRIBED` event; such requests will result in "403 Forbidden".

If the master does not realize that the subscription connection is broken, but the scheduler realizes it, the scheduler might open a new persistent connection to
"/scheduler" via `SUBSCRIBE`. In this case, the semantics depend on the value of `subscribe.force`. If set to true, master closes the existing subscription connection and allows subscription on the new connection. If set to false, the new connection attempt is disallowed in favor of the existing connection. The invariant here is that, only one persistent subscription connection for a given FrameworkID is allowed on the master. For HA schedulers, it is recommended that a scheduler instance set `subscribe.force` to true only when it just got elected and set it to false for all subsequent reconnection attempts (e.g, due to disconnection or master failover).

### Network partitions

In the case of a network partition, the subscription connection between the scheduler and master might not necessarily break. To be able to detect this scenario, master periodically (e.g., 15s) sends `HEARTBEAT` events (similar in vein to Twitter's Streaming API). If a scheduler doesn't receive a bunch (e.g., 5) of these heartbeats within a time window, it should immediately disconnect and try to re-subscribe. It is highly recommended for schedulers to use an exponential backoff strategy (e.g., upto a maximum of 15s) to avoid overwhelming the master while reconnecting. Schedulers can use a similar timeout (e.g., 75s) for receiving responses to any HTTP requests.

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

If the scheduler knows the list of master's hostnames for a cluster, it could use this mechanism to find the leading master to subscribe with. Alternatively, the scheduler could use a pure language library that detects the leading master given a ZooKeeper (or etcd) URL. For a `C++` library that does ZooKeeper based master detection please look at `src/scheduler/scheduler.cpp`.
