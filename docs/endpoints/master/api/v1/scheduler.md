---
title: Apache Mesos - HTTP Endpoints - /api/v1/scheduler
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /api/v1/scheduler
>        /master/api/v1/scheduler

### TL;DR; ###
Endpoint for schedulers to make calls against the master.

### DESCRIPTION ###
Returns 202 Accepted iff the request is accepted.
Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.
Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.