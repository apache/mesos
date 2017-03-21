---
title: Apache Mesos - HTTP Endpoints
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

# HTTP Endpoints #

Below is a list of HTTP endpoints available for a given Mesos process.

Depending on your configuration, some subset of these endpoints will be
available on your Mesos master or agent. Additionally, a `/help`
endpoint will be available that displays help similar to what you see
below.

** NOTE: ** If you are using Mesos 1.1 or later, we recommend using the
new [v1 Operator HTTP API](../operator-http-api.md) instead of the
unversioned REST endpoints listed below. These endpoints will be
deprecated in the future.


** NOTE: ** The documentation for these endpoints is auto-generated from
the Mesos source code. See `support/generate-endpoint-help.py`.

## Master Endpoints ##

Below are the endpoints that are available on a Mesos master. These
endpoints are reachable at the address `http://ip:port/endpoint`.

For example, `http://master.com:5050/files/browse`.

### files ###
* [/files/browse](files/browse.md)
* [/files/browse.json](files/browse.json.md)
* [/files/debug](files/debug.md)
* [/files/debug.json](files/debug.json.md)
* [/files/download](files/download.md)
* [/files/download.json](files/download.json.md)
* [/files/read](files/read.md)
* [/files/read.json](files/read.json.md)

### logging ###
* [/logging/toggle](logging/toggle.md)

### master ###
* [/api/v1](master/api/v1.md)
* [/api/v1/scheduler](master/api/v1/scheduler.md)
* [/create-volumes](master/create-volumes.md)
* [/destroy-volumes](master/destroy-volumes.md)
* [/flags](master/flags.md)
* [/frameworks](master/frameworks.md)
* [/health](master/health.md)
* [/machine/down](master/machine/down.md)
* [/machine/up](master/machine/up.md)
* [/maintenance/schedule](master/maintenance/schedule.md)
* [/maintenance/status](master/maintenance/status.md)
* [/quota](master/quota.md)
* [/redirect](master/redirect.md)
* [/reserve](master/reserve.md)
* [/roles](master/roles.md)
* [/roles.json](master/roles.json.md)
* [/slaves](master/slaves.md)
* [/state](master/state.md)
* [/state-summary](master/state-summary.md)
* [/state.json](master/state.json.md)
* [/tasks](master/tasks.md)
* [/tasks.json](master/tasks.json.md)
* [/teardown](master/teardown.md)
* [/unreserve](master/unreserve.md)
* [/weights](master/weights.md)

### metrics ###
* [/metrics/snapshot](metrics/snapshot.md)

### profiler ###
* [/profiler/start](profiler/start.md)
* [/profiler/stop](profiler/stop.md)

### registrar(id) ###
* [/registrar(id)/registry](registrar/registry.md)

### system ###
* [/system/stats.json](system/stats.json.md)

### version ###
* [/version](version.md)

## Agent Endpoints ##

Below are the endpoints that are available on a Mesos agent. These
endpoints are reachable at the address `http://ip:port/endpoint`.

For example, `http://agent.com:5051/files/browse`.

### files ###
* [/files/browse](files/browse.md)
* [/files/browse.json](files/browse.json.md)
* [/files/debug](files/debug.md)
* [/files/debug.json](files/debug.json.md)
* [/files/download](files/download.md)
* [/files/download.json](files/download.json.md)
* [/files/read](files/read.md)
* [/files/read.json](files/read.json.md)

### logging ###
* [/logging/toggle](logging/toggle.md)

### metrics ###
* [/metrics/snapshot](metrics/snapshot.md)

### profiler ###
* [/profiler/start](profiler/start.md)
* [/profiler/stop](profiler/stop.md)

### slave(id) ###
* [/api/v1](slave/api/v1.md)
* [/api/v1/executor](slave/api/v1/executor.md)
* [/containers](slave/containers.md)
* [/flags](slave/flags.md)
* [/health](slave/health.md)
* [/monitor/statistics](slave/monitor/statistics.md)
* [/monitor/statistics.json](slave/monitor/statistics.json.md)
* [/state](slave/state.md)
* [/state.json](slave/state.json.md)

### system ###
* [/system/stats.json](system/stats.json.md)

### version ###
* [/version](version.md)