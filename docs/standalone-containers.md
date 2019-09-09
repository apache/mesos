---
title: Apache Mesos - Standalone Containers
layout: documentation
---

# Standalone Containers

Traditionally, launching a container in a Mesos cluster involves
communication between multiple components:

```
                                                 Container(s)
  +-----------+     +--------+     +-------+     +----------+
  | Framework | <-> | Master | <-> | Agent | <-> | Executor |
  +-----------+     +--------+     +-------+     |  `->Task |
                         ^                       +----------+
                         |         +-------+     +----------+
                         +------>  | Agent | <-> | Executor |
                         |         +-------+     |  `->Task |
                        ...                      +----------+
```

Mesos 1.5 introduced "Standalone Containers", which provide an alternate
path for launching containers with a reduced scope and feature set:

```
                   +-------+    +----------------------+
  Operator API <-> | Agent | -> | Standalone Container |
                   +-------+    +----------------------+
```

**NOTE:** Agents currently require a connection to a Mesos master in
order to accept any Operator API calls.  This limitation is not necessary
and may be fixed in future.

**NOTE:** Standalone containers only apply to the Mesos containerizer.
For standalone docker containers, use docker directly.

As hinted by the diagrams, standalone containers are launched on single
Agents, rather than cluster-wide.  This document describes the major
differences between normal containers and standalone containers; and
provides some examples of how to use the new Operator APIs.


## Launching a Standalone Container

Because standalone containers are launched directly on Mesos Agents,
these containers do not participate in the Mesos Master's offer cycle.
This means standalone containers can be launched regardless of resource
allocation and can potentially overcommit the Mesos Agent, but cannot
use reserved resources.

An Operator API might look like this:

```
LAUNCH_CONTAINER HTTP Request (JSON):

POST /api/v1  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json

{
  "type": "LAUNCH_CONTAINER",
  "launch_container": {
    "container_id": {
      "value": "my-standalone-container-id"
    },
    "command": {
      "value": "sleep 100"
    },
    "resources": [
      {
        "name": "cpus",
        "scalar": { "value": 2.0 },
        "type": "SCALAR"
      },
      {
        "name": "mem",
        "scalar": { "value": 1024.0 },
        "type": "SCALAR"
      },
      {
        "name": "disk",
        "scalar": { "value": 1024.0 },
        "type": "SCALAR"
      }
    ],
    "container": {
      "type": "MESOS",
      "mesos": {
        "image": {
          "type": "DOCKER",
          "docker": {
            "name": "alpine"
          }
        }
      }
    }
  }
}
```

The Agent will return:

  * 200 OK if the launch succeeds, including fetching any container images
    or URIs specified in the launch command.
  * 202 Accepted if the specified ContainerID is already in use by a running
    container.
  * 400 Bad Request if the launch fails for any reason.

**NOTE:** Nested containers share the same Operator API.  To launch a nested
container, the ContainerID needs to have a parent; and no resources may be
specified in the request.


## Monitoring a Standalone Container

Standalone containers are not managed by a framework, do not use executors,
and therefore do not have status updates.  They are not automatically
relaunched upon completion/failure.

After launching a standalone container, the operator should monitor the
container via the `WAIT_CONTAINER` call:

```
WAIT_CONTAINER HTTP Request (JSON):

POST /api/v1  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json
Accept: application/json

{
  "type": "WAIT_CONTAINER",
  "wait_container": {
    "container_id": {
      "value": "my-standalone-container-id"
    }
  }
}

WAIT_CONTAINER HTTP Response (JSON):

HTTP/1.1 200 OK

Content-Type: application/json

{
  "type": "WAIT_CONTAINER",
  "wait_container": {
    "exit_status": 0
  }
}
```

This is a blocking HTTP call that only returns after the container has
exited.

If the specified ContainerID does not exist, the call returns a 404.


## Killing a Standalone Container

A standalone container can be signalled (usually to kill it) via this API:

```
KILL_CONTAINER HTTP Request (JSON):

POST /api/v1  HTTP/1.1

Host: agenthost:5051
Content-Type: application/json

{
  "type": "KILL_CONTAINER",
  "kill_container": {
    "container_id": {
      "value": "my-standalone-container-id"
    }
  }
}

KILL_CONTAINER HTTP Response (JSON):

HTTP/1.1 200 OK
```

If the specified ContainerID does not exist, the call returns a 404.


## Cleaning up a Standalone Container

Unlike other containers, a standalone container's sandbox is not garbage
collected by the Agent after some time (like other sandbox directories).
The Agent is unable to garbage collect these containers because there is
no status update mechanism to report the exit status of the container.

Standalone container sandboxes must be manually cleaned up by the operator and
are located in the agent's work directory under
`/containers/<my-standalone-container-id>`.
