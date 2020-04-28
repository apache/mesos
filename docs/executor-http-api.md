---
title: Apache Mesos - Executor HTTP API
layout: documentation
---

# Executor HTTP API

A Mesos executor can be built in two different ways:

1. By using the HTTP API. This allows Mesos executors to be developed without
using C++ or a native client library; instead, a custom executor interacts with
the Mesos agent via HTTP requests, as described below. Although it is
theoretically possible to use the HTTP executor API "directly" (e.g., by using a
generic HTTP library), most executor developers should use a library for their
language of choice that manages the details of the HTTP API; see the document on
[HTTP API client libraries](api-client-libraries.md) for a list. This is the
recommended way to develop new Mesos executors.

2. By using the deprecated `ExecutorDriver` C++ interface. While this interface
is still supported, note that new features are usually not added to it. The
`ExecutorDriver` handles the details of communicating with the Mesos agent.
Executor developers implement custom executor logic by registering callbacks
with the `ExecutorDriver` for significant events, such as when a new task launch
request is received. Because the `ExecutorDriver` interface is written in C++,
this typically requires that executor developers either use C++ or use a C++
binding to their language of choice (e.g., JNI when using JVM-based languages).


## Overview

The executor interacts with Mesos via the [/api/v1/executor]
(endpoints/slave/api/v1/executor.md) agent endpoint. We refer to this endpoint
with its suffix "/executor" in the rest of this document. The endpoint accepts
HTTP POST requests with data encoded as JSON (Content-Type: application/json) or
binary Protobuf (Content-Type: application/x-protobuf). The first request that
the executor sends to the "/executor" endpoint is called `SUBSCRIBE` and results
in a streaming response ("200 OK" status code with Transfer-Encoding: chunked).

**Executors are expected to keep the subscription connection open as long as
possible (barring network errors, agent process restarts, software bugs, etc.)
and incrementally process the response.** HTTP client libraries that can only
parse the response after the connection is closed cannot be used. For the
encoding used, please refer to **Events** section below.

All subsequent (non-`SUBSCRIBE`) requests to the "/executor" endpoint (see
details below in **Calls** section) must be sent using a different connection
than the one used for subscription. The agent responds to these HTTP POST
requests with "202 Accepted" status codes (or, for unsuccessful requests, with
4xx or 5xx status codes; details in later sections). The "202 Accepted" response
means that a request has been accepted for processing, not that the processing
of the request has been completed. The request might or might not be acted upon
by Mesos (e.g., agent fails during the processing of the request). Any
asynchronous responses from these requests will be streamed on the long-lived
subscription connection. Executors can submit requests using more than one
different HTTP connection.

The "/executor" endpoint is served at the Mesos agent's IP:port and in addition,
when the agent has the `http_executor_domain_sockets` flag set to `true`, the
executor endpoint is also served on a Unix domain socket, the location of which
can be found by the executor in the `MESOS_DOMAIN_SOCKET` environment variable.
Connecting to the domain socket is similar to connecting using a TCP socket, and
once the connection is established, data is sent and received in the same way.

## Calls

The following calls are currently accepted by the agent. The canonical source of
this information is [executor.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/executor/executor.proto).
When sending JSON-encoded Calls, executors should encode raw bytes in Base64 and
strings in UTF-8.

### SUBSCRIBE

This is the first step in the communication process between the executor and
agent. This is also to be considered as subscription to the "/executor" events
stream.

To subscribe with the agent, the executor sends an HTTP POST with a `SUBSCRIBE`
message. The HTTP response is a stream in [RecordIO]
(scheduler-http-api.md#recordio-response-format) format; the event stream will
begin with a `SUBSCRIBED` event (see details in **Events** section).

Additionally, if the executor is connecting to the agent after a
[disconnection](#disconnections), it can also send a list of:

* **Unacknowledged Status Updates**: The executor is expected to maintain a list
  of status updates not acknowledged by the agent via the `ACKNOWLEDGE` events.
* **Unacknowledged Tasks**: The executor is expected to maintain a list of tasks
  that have not been acknowledged by the agent. A task is considered
  acknowledged if at least one of the status updates for this task is
  acknowledged by the agent.

```
SUBSCRIBE Request (JSON):

POST /api/v1/executor  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json
Accept: application/json

{
  "type": "SUBSCRIBE",
  "executor_id": {
    "value": "387aa966-8fc5-4428-a794-5a868a60d3eb"
  },
  "framework_id": {
    "value": "49154f1b-8cf6-4421-bf13-8bd11dccd1f1"
  },
  "subscribe": {
    "unacknowledged_tasks": [
      {
        "name": "dummy-task",
        "task_id": {
          "value": "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"
        },
        "agent_id": {
          "value": "f1c9cdc5-195e-41a7-a0d7-adaa9af07f81"
        },
        "command": {
          "value": "ls",
          "arguments": [
            "-l",
            "\/tmp"
          ]
        }
      }
    ],
    "unacknowledged_updates": [
      {
        "framework_id": {
          "value": "49154f1b-8cf6-4421-bf13-8bd11dccd1f1"
        },
        "status": {
          "source": "SOURCE_EXECUTOR",
          "task_id": {
            "value": "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"
          },
        "state": "TASK_RUNNING",
        "uuid": "ZDQwZjNmM2UtYmJlMy00NGFmLWEyMzAtNGNiMWVhZTcyZjY3Cg=="
        }
      }
    ]
  }
}

SUBSCRIBE Response Event (JSON):
HTTP/1.1 200 OK

Content-Type: application/json
Transfer-Encoding: chunked

<event-length>
{
  "type": "SUBSCRIBED",
  "subscribed": {
    "executor_info": {
      "executor_id": {
        "value": "387aa966-8fc5-4428-a794-5a868a60d3eb"
      },
      "command": {
        "value": "\/path\/to\/executor"
      },
      "framework_id": {
        "value": "49154f1b-8cf6-4421-bf13-8bd11dccd1f1"
      }
    },
    "framework_info": {
      "user": "foo",
      "name": "my_framework"
    },
    "agent_id": {
      "value": "f1c9cdc5-195e-41a7-a0d7-adaa9af07f81"
    },
    "agent_info": {
      "host": "agenthost",
      "port": 5051
    }
  }
}
<more events>
```

NOTE: Once an executor is launched, the agent waits for a duration of `--executor_registration_timeout` (configurable at agent startup) for the executor to subscribe. If the executor fails to subscribe within this duration, the agent forcefully destroys the container executor is running in.

### UPDATE

Sent by the executor to reliably communicate the state of managed tasks. It is crucial that a terminal update (e.g., `TASK_FINISHED`, `TASK_KILLED` or `TASK_FAILED`) is sent to the agent as soon as the task terminates, in order to allow Mesos to release the resources allocated to the task.

The scheduler must explicitly respond to this call through an `ACKNOWLEDGE` message (see `ACKNOWLEDGED` in the Events section below for the semantics). The executor must maintain a list of unacknowledged updates. If for some reason, the executor is disconnected from the agent, these updates must be sent as part of `SUBSCRIBE` request in the `unacknowledged_updates` field.

```
UPDATE Request (JSON):

POST /api/v1/executor  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json
Accept: application/json

{
  "executor_id": {
    "value": "387aa966-8fc5-4428-a794-5a868a60d3eb"
  },
  "framework_id": {
    "value": "9aaa9d0d-e00d-444f-bfbd-23dd197939a0-0000"
  },
  "type": "UPDATE",
  "update": {
    "status": {
      "executor_id": {
        "value": "387aa966-8fc5-4428-a794-5a868a60d3eb"
      },
      "source": "SOURCE_EXECUTOR",
      "state": "TASK_RUNNING",
      "task_id": {
        "value": "66724cec-2609-4fa0-8d93-c5fb2099d0f8"
      },
      "uuid": "ZDQwZjNmM2UtYmJlMy00NGFmLWEyMzAtNGNiMWVhZTcyZjY3Cg=="
    }
  }
}

UPDATE Response:
HTTP/1.1 202 Accepted
```

### MESSAGE

Sent by the executor to send arbitrary binary data to the scheduler. Note that Mesos neither interprets this data nor makes any guarantees about the delivery of this message to the scheduler. The `data` field is raw bytes encoded in Base64.

```
MESSAGE Request (JSON):

POST /api/v1/executor  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json
Accept: application/json

{
  "executor_id": {
    "value": "387aa966-8fc5-4428-a794-5a868a60d3eb"
  },
  "framework_id": {
    "value": "9aaa9d0d-e00d-444f-bfbd-23dd197939a0-0000"
  },
  "type": "MESSAGE",
  "message": {
    "data": "t+Wonz5fRFKMzCnEptlv5A=="
  }
}

MESSAGE Response:
HTTP/1.1 202 Accepted
```

## Events

Executors are expected to keep a **persistent** connection to the "/executor" endpoint (even after getting a `SUBSCRIBED` HTTP Response event). This is indicated by the "Connection: keep-alive" and "Transfer-Encoding: chunked" headers with *no* "Content-Length" header set. All subsequent events that are relevant to this executor generated by Mesos are streamed on this connection. The agent encodes each Event in [RecordIO](scheduler-http-api.md#recordio-response-format) format, i.e., string representation of length of the event in bytes followed by JSON or binary Protobuf  (possibly compressed) encoded event. The length of an event is a 64-bit unsigned integer (encoded as a textual value) and will never be "0". Also, note that the `RecordIO` encoding should be decoded by the executor whereas the underlying HTTP chunked encoding is typically invisible at the application (executor) layer. The type of content encoding used for the events will be determined by the accept header of the POST request (e.g., "Accept: application/json").

The following events are currently sent by the agent. The canonical source of this information is at [executor.proto](https://github.com/apache/mesos/blob/master/include/mesos/v1/executor/executor.proto). Note that when sending JSON-encoded events, agent encodes raw bytes in Base64 and strings in UTF-8.

### SUBSCRIBED

The first event sent by the agent when the executor sends a `SUBSCRIBE` request on the persistent connection. See `SUBSCRIBE` in Calls section for the format.

### LAUNCH

Sent by the agent whenever it needs to assign a new task to the executor. The executor is required to send an `UPDATE` message back to the agent indicating the success or failure of the task initialization.

The executor must maintain a list of unacknowledged tasks (see `SUBSCRIBE` in `Calls` section). If for some reason, the executor is disconnected from the agent, these tasks must be sent as part of `SUBSCRIBE` request in the `tasks` field.

```
LAUNCH Event (JSON)

<event-length>
{
  "type": "LAUNCH",
  "launch": {
    "framework_info": {
      "id": {
        "value": "49154f1b-8cf6-4421-bf13-8bd11dccd1f1"
      },
      "user": "foo",
      "name": "my_framework"
    },
    "task": {
      "name": "dummy-task",
      "task_id": {
        "value": "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"
      },
      "agent_id": {
        "value": "f1c9cdc5-195e-41a7-a0d7-adaa9af07f81"
      },
      "command": {
        "value": "sleep",
        "arguments": [
          "100"
        ]
      }
    }
  }
}
```

### LAUNCH_GROUP

This **experimental** event was added in 1.1.0.

Sent by the agent whenever it needs to assign a new task group to the executor. The executor is required to send `UPDATE` messages back to the agent indicating the success or failure of each of the tasks in the group.

The executor must maintain a list of unacknowledged tasks (see `LAUNCH` section above).

```
LAUNCH_GROUP Event (JSON)

<event-length>
{
  "type": "LAUNCH_GROUP",
  "launch_group": {
    "task_group" : {
      "tasks" : [
        {
          "name": "dummy-task",
          "task_id": {
            "value": "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"
          },
          "agent_id": {
            "value": "f1c9cdc5-195e-41a7-a0d7-adaa9af07f81"
          },
          "command": {
            "value": "sleep",
            "arguments": [
              "100"
            ]
          }
        }
      ]
    }
  }
}
```

### KILL

The `KILL` event is sent whenever the scheduler needs to stop execution of a specific task. The executor is required to send a terminal update (e.g., `TASK_FINISHED`, `TASK_KILLED` or `TASK_FAILED`) back to the agent once it has stopped/killed the task. Mesos will mark the task resources as freed once the terminal update is received.

```
LAUNCH Event (JSON)

<event-length>
{
  "type" : "KILL",
  "kill" : {
    "task_id" : {"value" : "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"}
  }
}
```

### ACKNOWLEDGED

Sent by the agent in order to signal the executor that a status update was received as part of the reliable message passing mechanism. Acknowledged updates must not be retried.

```
ACKNOWLEDGED Event (JSON)

<event-length>
{
  "type" : "ACKNOWLEDGED",
  "acknowledged" : {
    "task_id" : {"value" : "d40f3f3e-bbe3-44af-a230-4cb1eae72f67"},
    "uuid" : "ZDQwZjNmM2UtYmJlMy00NGFmLWEyMzAtNGNiMWVhZTcyZjY3Cg=="
  }
}
```

### MESSAGE

Custom message generated by the scheduler and forwarded all the way to the executor. These messages are delivered "as-is" by Mesos and have no delivery guarantees. It is up to the scheduler to retry if a message is dropped for any reason. The `data` field contains raw bytes encoded as Base64.

```
MESSAGE Event (JSON)

<event-length>
{
  "type" : "MESSAGE",
  "message" : {
    "data" : "c2FtcGxlIGRhdGE="
  }
}
```

### SHUTDOWN

Sent by the agent in order to shutdown the executor. Once an executor gets a `SHUTDOWN` event it is required to kill all its tasks, send `TASK_KILLED` updates and gracefully exit. If an executor doesn't terminate within a certain period `MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD` (an environment variable set by the agent upon executor startup), the agent will forcefully destroy the container where the executor is running. The agent would then send `TASK_LOST` updates for any remaining active tasks of this executor.

```
SHUTDOWN Event (JSON)

<event-length>
{
  "type" : "SHUTDOWN"
}
```

### ERROR

Sent by the agent when an asynchronous error event is generated. It is recommended that the executor abort when it receives an error event and retry subscription.

```
ERROR Event (JSON)

<event-length>
{
  "type" : "ERROR",
  "error" : {
    "message" : "Unrecoverable error"
  }
}
```

## Executor Environment Variables

The following environment variables are set by the agent that can be used by the executor upon startup:

* `MESOS_FRAMEWORK_ID`: `FrameworkID` of the scheduler needed as part of the `SUBSCRIBE` call.
* `MESOS_EXECUTOR_ID`: `ExecutorID` of the executor needed as part of the `SUBSCRIBE` call.
* `MESOS_DIRECTORY`: Path to the working directory for the executor on the host filesystem (deprecated).
* `MESOS_SANDBOX`: Path to the mapped sandbox inside of the container (determined by the agent flag `sandbox_directory`) for either mesos container with image or docker container. For the case of command task without image specified, it is the path to the sandbox on the host filesystem, which is identical to `MESOS_DIRECTORY`. `MESOS_DIRECTORY` is always the sandbox on the host filesystem.
* `MESOS_AGENT_ENDPOINT`: Agent endpoint (i.e., ip:port to be used by the executor to connect to the agent).
* `MESOS_CHECKPOINT`: If set to true, denotes that framework has checkpointing enabled.
* `MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD`: Amount of time the agent would wait for an executor to shut down (e.g., 60secs, 3mins etc.) after sending a `SHUTDOWN` event.
* `MESOS_EXECUTOR_AUTHENTICATION_TOKEN`: The token the executor should use to authenticate with the agent. When executor authentication is enabled, the agent generates a JSON web token (JWT) that the executor can use to authenticate with the agent's default JWT authenticator.

If `MESOS_CHECKPOINT` is set (i.e., if framework checkpointing is enabled), the following additional variables are also set that can be used by the executor for retrying upon a disconnection with the agent:

* `MESOS_RECOVERY_TIMEOUT`: The total duration that the executor should spend retrying before shutting itself down when it is disconnected from the agent (e.g., `15mins`, `5secs` etc.). This is configurable at agent startup via the flag `--recovery_timeout`.
* `MESOS_SUBSCRIPTION_BACKOFF_MAX`: The maximum backoff duration to be used by the executor between two retries when disconnected (e.g., `250ms`, `1mins` etc.). This is configurable at agent startup via the flag `--executor_reregistration_timeout`.

NOTE: Additionally, the executor also inherits all the agent's environment variables.

<a name="disconnections"></a>
## Disconnections

An executor considers itself disconnected if the persistent subscription connection (opened via SUBSCRIBE request) to "/executor" breaks. The disconnection can happen due to an agent process failure etc.

Upon detecting a disconnection from the agent, the retry behavior depends on whether framework checkpointing is enabled:

* If framework checkpointing is disabled, the executor is not supposed to retry subscription and gracefully exit.
* If framework checkpointing is enabled, the executor is supposed to retry subscription using a suitable [backoff strategy](#backoff-strategies) for a duration of `MESOS_RECOVERY_TIMEOUT`. If it is not able to establish a subscription with the agent within this duration, it should gracefully exit.

## Agent Recovery

Upon agent startup, an agent performs [recovery](agent-recovery.md). This allows the agent to recover status updates and reconnect with old executors. Currently, the agent supports the following recovery mechanisms specified via the `--recover` flag:

* **reconnect** (default): This mode allows the agent to reconnect with any of it's old live executors provided the framework has enabled checkpointing. The recovery of the agent is only marked complete once all the disconnected executors have connected and hung executors have been destroyed. Hence, it is mandatory that every executor retries at least once within the interval (`MESOS_SUBSCRIPTION_BACKOFF_MAX`) to ensure it is not shutdown by the agent due to being hung/unresponsive.
* **cleanup**: This mode kills any old live executors and then exits the agent. This is usually done by operators when making a non-compatible agent/executor upgrade. Upon receiving a `SUBSCRIBE` request from the executor of a framework with checkpointing enabled, the agent would send it a `SHUTDOWN` event as soon as it reconnects. For hung executors, the agent would wait for a duration of `--executor_shutdown_grace_period` (configurable at agent startup) and then forcefully kill the container where the executor is running in.

<a name="backoff-strategies"></a>
## Backoff Strategies

Executors are encouraged to retry subscription using a suitable backoff strategy like linear backoff, when they notice a disconnection with the agent. A disconnection typically happens when the agent process terminates (e.g., restarted for an upgrade). Each retry interval should be bounded by the value of `MESOS_SUBSCRIPTION_BACKOFF_MAX` which is set as an environment variable.
