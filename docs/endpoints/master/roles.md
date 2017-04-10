---
title: Apache Mesos - HTTP Endpoints - /roles
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /roles
>        /master/roles

### TL;DR; ###
Information about roles.

### DESCRIPTION ###
Returns 200 OK when information about roles was queried successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

This endpoint provides information about roles as a JSON object.
It returns information about every role that is on the role
whitelist (if enabled), has one or more registered frameworks,
or has a non-default weight or quota. For each role, it returns
the weight, total allocated resources, and registered frameworks.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.