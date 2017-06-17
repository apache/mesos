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

### AUTHORIZATION ###
GET: The response will contain only the maintenance schedule for
those machines the current principal is allowed to see. If none
an empty response will be returned.

POST: The current principal must be authorized to modify the
maintenance schedule of all the machines in the request. If the
principal is unauthorized to modify the schedule for at least one
machine, the whole request will fail.