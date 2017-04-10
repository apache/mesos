---
title: Apache Mesos - HTTP Endpoints - /maintenance/schedule
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /maintenance/schedule
>        /master/maintenance/schedule

### TL;DR; ###
Returns or updates the cluster's maintenance schedule.

### DESCRIPTION ###
Returns 200 OK when the requested maintenance operation was performed
successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

GET: Returns the current maintenance schedule as JSON.

POST: Validates the request body as JSON
and updates the maintenance schedule.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.