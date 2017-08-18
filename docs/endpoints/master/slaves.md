---
title: Apache Mesos - HTTP Endpoints - /slaves
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /slaves
>        /master/slaves

### TL;DR; ###
Information about agents.

### DESCRIPTION ###
Returns 200 OK when the request was processed successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

This endpoint shows information about the agents which are registered
in this master or recovered from registry, formatted as a JSON
object.

Query parameters:
>        slave_id=VALUE       The ID of the slave returned (when no slave_id is specified, all slaves will be returned).


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.